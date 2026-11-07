// Integration against the production C LDN implementation and Rust emulator transport.
#include <stdarg.h>
#include <assert.h>
typedef int socklen_t;
#include "../../runtime/net/ldn.c"
_Static_assert(PROTO_VERSION == 2, "The original C/Go hub protocol must remain compatible");

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static KEvent state_event = {KOBJ_EVENT, 1, false};
static LONG event_count, down_count, packet_count, em_count, hp_count;
static VPacket last_packet;
static uint8_t last_data[256];
static uint32_t last_size;
static uint32_t guest_call_args[4];

uint64_t plat_now_ns(void) { LARGE_INTEGER n, f; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f); return (uint64_t)((n.QuadPart / f.QuadPart) * 1000000000ull + (n.QuadPart % f.QuadPart) * 1000000000ull / f.QuadPart); }
void plat_sleep_until_ns(uint64_t end) { uint64_t now = plat_now_ns(); if (end > now) Sleep((DWORD)((end - now + 999999) / 1000000)); }
void kernel_signal(KObject *o) { o->signaled = true; InterlockedIncrement(&event_count); }
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) { *w = 0x100; *r = 0x101; return &state_event; }
uint32_t guest_sym(const char *name) { assert(strstr(name, "AttachReadableHandleToSystemEvent")); return 0x1234; }
void guest_call(Cpu *c, uint32_t addr) { assert(addr == 0x1234); memcpy(guest_call_args, c->r, sizeof guest_call_args); }
void overlay_notice(const char *title, const char *msg) { fprintf(stderr, "NOTICE %s: %s\n", title, msg); }
void vsock_deliver(const VPacket *p, const void *data, uint32_t n) { fprintf(stderr,"DELIVER %x:%u -> %x:%u %.*s\n",p->src_ip,p->src_port,p->dst_ip,p->dst_port,(int)n,(const char *)data); assert(n <= sizeof last_data); last_packet = *p; memcpy(last_data, data, n); last_size = n; InterlockedIncrement(&packet_count); }
void vsock_peer_gone(uint32_t ip) { (void)ip; }
void vsock_network_down(void) { InterlockedIncrement(&down_count); }
void emsync_aux_received(const void *data, uint32_t n) { (void)data; (void)n; InterlockedIncrement(&em_count); }
void hpscale_aux_received(const void *data, uint32_t n) { (void)data; (void)n; InterlockedIncrement(&hp_count); }

enum { A_CFG=0x10000, A_SEC=0x10100, A_USER=0x10200, A_INFO=0x11000, A_STACK=0x1f000, A_FILTER=0x16000, A_COUNT=0x16100 };
static Cpu cpu;
static bool emulator, server_host;
static int transport;
static uint32_t call(HostFn fn, uint32_t a, uint32_t b, uint32_t d, uint32_t e) { cpu.r[0]=a; cpu.r[1]=b; cpu.r[2]=d; cpu.r[3]=e; cpu.r[13]=A_STACK; fn(&cpu); return cpu.r[0]; }
static int current_state(void) { return (int)call(ldn_get_state,0,0,0,0); }
static void prepare(int port, const char *name) {
    assert(VirtualAlloc((void *)(uintptr_t)GUEST_BASE, 0x100000, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE) == (void *)(uintptr_t)GUEST_BASE);
    Config c = g_config; c.ldn_port = port; c.crossplay = XP_OFF; c.ryu_host = c.room_host = 0;
    strcpy(c.ldn_server, "Off"); snprintf(c.ldn_join,sizeof c.ldn_join,"127.0.0.1:%d", port); c.ryu_servers[0]=c.room_server[0]=0;
    if (emulator) { c.crossplay=transport; c.ryu_host=server_host && transport!=XP_ROOM; c.room_host=server_host && transport!=XP_RYU; c.ryu_port=c.room_port=port; snprintf(c.ryu_servers,sizeof c.ryu_servers,"127.0.0.1:%d",port); snprintf(c.room_server,sizeof c.room_server,"127.0.0.1:%d",port); snprintf(c.room_name,sizeof c.room_name,"%s",name); }
    g_config=c; config_store_loaded(&c); config_runtime_init(&c);
    uint8_t *cfg = G2H(A_CFG); wr32(cfg, 0x11223344); wr16(cfg+0x10,6); cfg[0x12]=4; wr16(cfg+0x14,0x1234);
    strncpy(G2H(A_USER),name,0x20);
    assert(call(ldn_initialize,0,0,0,0)==0 && current_state()==ST_INITIALIZED);
    call(ldn_attach_event,0x17000,0,0,0);
    assert(guest_call_args[0]==0x17000 && guest_call_args[1]==0x101 && guest_call_args[2]==0 && guest_call_args[3]==1);
}
static uint8_t *network_info(int nodes) {
    memset(G2H(A_INFO),0xcc,NETINFO_SIZE); assert(call(ldn_get_network_info,A_INFO,0,0,0)==0);
    uint8_t *p=G2H(A_INFO); assert(info_sane(p)); assert(p[NI_NODE_COUNT]==nodes && p[NI_NODE_MAX]==4);
    assert(rd32(p+NI_LOCAL_COMM_ID)==0x11223344);
    for(int i=0;i<nodes;i++) { uint8_t *nd=node_at(p,i); assert(nd[NODE_CONNECTED] && nd[NODE_ID]==i && rd16(nd+NODE_LCV)==0x1234); }
    return p;
}
static void create(void) { assert(call(ldn_open_ap,0,0,0,0)==0); uint32_t r=call(ldn_create,A_CFG,A_SEC,A_USER,0); for(int w=0; transport==XP_ROOM && r && w<120; w++) { assert(current_state()==ST_ACCESS_POINT); Sleep(50); r=call(ldn_create,A_CFG,A_SEC,A_USER,0); } assert(r==0); assert(current_state()==ST_AP_CREATED && ldn_net_up() && ldn_hosting()); network_info(1); }
static void destroy(void) { assert(call(ldn_destroy,0,0,0,0)==0); assert(current_state()==ST_ACCESS_POINT && !ldn_net_up()); assert(call(ldn_get_network_info,A_INFO,0,0,0)==R_INVALID_STATE); }
static void finish(void) { call(ldn_finalize,0,0,0,0); assert(current_state()==ST_NONE && !ldn_net_up() && !room_keep_started); VirtualFree((void *)(uintptr_t)GUEST_BASE,0,MEM_RELEASE); }
static int lifecycle(int port) {
    prepare(port,"HLE host"); hnet_init(); SOCKET occupied=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); assert(occupied!=INVALID_SOCKET);
    BOOL one=TRUE; assert(setsockopt(occupied,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(const char *)&one,sizeof one)==0);
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_ANY); a.sin_port=htons((uint16_t)port);
    assert(bind(occupied,(struct sockaddr *)&a,sizeof a)==0 && listen(occupied,2)==0);
    assert(call(ldn_open_ap,0,0,0,0)==0);
    assert(call(ldn_create,A_CFG,A_SEC,A_USER,0)==R_CONNECT_FAILURE);
    assert(current_state()==ST_ACCESS_POINT && !ldn_net_up() && !L.own_ip);
    puts("PASS occupied-port CreateNetwork returns R_CONNECT_FAILURE and keeps access-point state"); closesocket(occupied);
    assert(call(ldn_create,A_CFG,A_SEC,A_USER,0)==0); network_info(1);
    assert(call(ldn_get_ipv4,0x18000,0x18004,0,0)==0 && LD32(0x18000)==L.own_ip && LD32(0x18004)==0xffffff00);
    assert(call(ldn_get_net_config,0x18010,0,0,0)==0 && LD16(0x18024)==0x1234);
    puts("PASS Initialize/OpenAccessPoint/CreateNetwork/GetNetworkInfo/GetIpv4Address/GetNetworkConfig/AttachStateChangeEvent");
    destroy(); assert(call(ldn_close_ap,0,0,0,0)==0 && current_state()==ST_INITIALIZED);
    create(); destroy(); finish();
    prepare(port,"HLE reinitialized"); create(); destroy(); finish();
    printf("PASS DestroyNetwork/Finalize/reinitialize/rehost (state events=%ld, network-down=%ld)\n",event_count,down_count); return 0;
}
static int host(int port, const char *ready, const char *stop) {
    prepare(port,"HLE host"); create(); FILE *f=fopen(ready,"wb"); assert(f); fputs("ready",f); fclose(f);
    bool joined=false, relayed=false; uint64_t end=plat_now_ns()+20000000000ull;
    while(plat_now_ns()<end && GetFileAttributesA(stop)==INVALID_FILE_ATTRIBUTES) {
        int n=0,max=0; char id[32]; if(ldn_party(&n,&max,id,sizeof id) && n==2) { network_info(2); joined=true; }
        if(InterlockedCompareExchange(&packet_count,0,0)>0 && !relayed) {
            assert(last_size==4 && !memcmp(last_data,"ping",4)); VPacket p=last_packet;
            uint32_t ip=p.src_ip; p.src_ip=p.dst_ip; p.dst_ip=ip; uint16_t pp=p.src_port; p.src_port=p.dst_port; p.dst_port=pp;
            ldn_send(&p,"pong",4); ldn_send_aux("host-aux",8); relayed=true;
        } Sleep(10);
    }
    assert(joined && relayed); destroy(); finish(); puts("PASS host observes joined node; C HLE packet and appropriate AUX relay; DestroyNetwork"); return 0;
}
static int station(int port, const char *stop) {
    prepare(port,"HLE station"); assert(call(ldn_open_station,0,0,0,0)==0); ST32(A_STACK,0);
    assert(call(ldn_scan,A_INFO,A_COUNT,4,A_FILTER)==0 && LD32(A_COUNT)>=1);
    uint8_t selected[NETINFO_SIZE]; memcpy(selected,G2H(A_INFO),NETINFO_SIZE); assert(info_sane(selected));
    assert(call(ldn_connect,A_INFO,A_SEC,A_USER,0x1234)==0 && current_state()==ST_STATION_CONNECTED);
    assert(ldn_crossplay()==emulator);
    if(transport==XP_RYU) assert(ldn_broadcast_ip()==0x0a72ffffu);
    uint8_t *info=network_info(2); uint32_t host_ip=rd32(node_at(info,0)+NODE_IP);
    assert(L.own_ip && L.own_ip!=host_ip); VPacket p={L.own_ip,host_ip,5500,6600,VPROTO_UDP,0,0}; ldn_send(&p,"ping",4);
    uint64_t end=plat_now_ns()+5000000000ull; while(plat_now_ns()<end && (!packet_count || (!emulator && (!em_count || !hp_count)))) Sleep(10);
    assert(packet_count && last_size==4 && !memcmp(last_data,"pong",4)); assert(last_packet.dst_ip==L.own_ip); if (emulator) assert(!em_count && !hp_count); else assert(em_count && hp_count);
    puts("PASS Scan/Connect/GetNetworkInfo, virtual packet roundtrip and both AUX handlers");
    FILE *f=fopen(stop,"wb"); assert(f); fputs("stop",f); fclose(f);
    end=plat_now_ns()+5000000000ull; while(plat_now_ns()<end && current_state()==ST_STATION_CONNECTED) Sleep(10);
    if(transport==XP_ROOM) assert(current_state()==ST_STATION && call(ldn_get_reason,0,0,0,0)==DR_DESTROYED_BY_SYSTEM && down_count);
    else assert(current_state()==ST_ERROR && call(ldn_get_reason,0,0,0,0)==(emulator ? DR_SIGNAL_LOST : DR_DESTROYED_BY_USER) && down_count);
    assert(call(ldn_get_network_info,A_INFO,0,0,0)==R_INVALID_STATE);
    if(current_state()==ST_ERROR) assert(call(ldn_disconnect,0,0,0,0)==0 && current_state()==ST_STATION); finish();
    puts("PASS host destroy propagates to ST_ERROR with the transport disconnect reason and closes virtual network; disconnect/finalize"); return 0;
}
static int edge_cases(int port) {
    prepare(port,"edge cases");
    Config config=g_config; config.crossplay=XP_ROOM; config.room_host=0; snprintf(config.room_server,sizeof config.room_server,"127.0.0.1:%d",port);
    config_store_loaded(&config); config_runtime_init(&config);
    assert(call(ldn_open_ap,0,0,0,0)==0);
    assert(call(ldn_create,A_CFG,A_SEC,A_USER,0)==R_CONNECT_FAILURE);
    assert(current_state()==ST_ACCESS_POINT && !L.hosting && !ldn_net_up());
    puts("PASS unavailable Room never creates a native C hub");
    room_shutdown();
    L.room=true; L.state=ST_AP_CREATED; L.info[NI_NODE_COUNT]=2; L.info[NI_NODE_MAX]=4;
    uint8_t *nd=node_at(L.info,1); memset(nd,0,NODE_SIZE); nd[NODE_CONNECTED]=1; L.room_client[1]=true;
    uint8_t departed[4]={192,168,0,2}; memcpy(L.room_cip[1],departed,4);
    room_members_changed(NULL,0);
    assert(!L.room_client[1] && !nd[NODE_CONNECTED] && L.info[NI_NODE_COUNT]==1);
    puts("PASS Room membership update releases a vanished peer's player slot");
    L.room=false; L.state=ST_ACCESS_POINT;
    finish(); return 0;
}
static int external_server(int port, const char *ready, const char *stop) {
    char err[128]; assert(mhgu_ryusrv_start("127.0.0.1",(uint16_t)port,err,sizeof err));
    FILE *f=fopen(ready,"wb"); assert(f); fclose(f);
    uint64_t end=plat_now_ns()+20000000000ull;
    while(plat_now_ns()<end && GetFileAttributesA(stop)==INVALID_FILE_ATTRIBUTES) Sleep(10);
    mhgu_ryusrv_stop(); return 0;
}
static int remote_host(int port) {
    prepare(port,"remote host"); assert(!g_config.ryu_host && !mhgu_ryusrv_running()); create();
    assert(L.ryu && L.own_ip && L.ryu_mask==0xffff0000u);
    destroy(); finish(); puts("PASS RyuHost Off creates a hub through the first RyuServers entry"); return 0;
}
static void join_any(void) {
    assert(call(ldn_open_station,0,0,0,0)==0); ST32(A_STACK,0);
    for(int w=0;w<10;w++) {
        assert(call(ldn_scan,A_INFO,A_COUNT,4,A_FILTER)==0);
        if(LD32(A_COUNT)>0) break;
        Sleep(100);
    }
    assert(LD32(A_COUNT)>0);
    assert(call(ldn_connect,A_INFO,A_SEC,A_USER,0x1234)==0);
}
static int three_host(int port,const char *ready,const char *stop) {
    prepare(port,"three-host"); create();
    FILE *f=fopen(ready,"wb"); assert(f); fclose(f);
    bool joined=false; uint64_t end=plat_now_ns()+20000000000ull;
    while(plat_now_ns()<end && GetFileAttributesA(stop)==INVALID_FILE_ATTRIBUTES) {
        if(L.info[NI_NODE_COUNT]==3) joined=true;
        Sleep(10);
    }
    assert(joined); destroy(); finish(); puts("PASS Mixed host bridged an ordinary Ryu player and a Room player"); return 0;
}
static int three_peer(int port,const char *stop) {
    prepare(port,transport==XP_RYU ? "three-ryu" : "three-room"); join_any();
    uint64_t end=plat_now_ns()+10000000000ull;
    while(plat_now_ns()<end && L.info[NI_NODE_COUNT]!=3) Sleep(10);
    assert(L.info[NI_NODE_COUNT]==3);
    if(transport==XP_RYU) {
        uint32_t target=0;
        for(int i=1;i<MAX_NODES;i++) { uint8_t *nd=node_at(L.info,i); if(nd[NODE_CONNECTED] && rd32(nd+NODE_IP)!=L.own_ip) target=rd32(nd+NODE_IP); }
        assert(target); VPacket p={L.own_ip,target,7000,7001,VPROTO_UDP,0,0}; ldn_send(&p,"ping",4);
    }
    end=plat_now_ns()+5000000000ull; while(plat_now_ns()<end && !packet_count) Sleep(10);
    assert(packet_count && last_size==4);
    if(transport==XP_ROOM) {
        assert(!memcmp(last_data,"ping",4) && (last_packet.src_ip&0xffffff00u)==0xc0a8fa00u);
        VPacket p=last_packet; uint32_t ip=p.src_ip;p.src_ip=p.dst_ip;p.dst_ip=ip;
        uint16_t pp=p.src_port;p.src_port=p.dst_port;p.dst_port=pp;ldn_send(&p,"pong",4);
        while(plat_now_ns()<end && GetFileAttributesA(stop)==INVALID_FILE_ATTRIBUTES) Sleep(10);
    } else {
        assert(!memcmp(last_data,"pong",4)); FILE *f=fopen(stop,"wb");assert(f);fclose(f);
    }
    if(current_state()==ST_STATION_CONNECTED || current_state()==ST_ERROR) call(ldn_disconnect,0,0,0,0);
    finish(); puts("PASS ordinary Ryu player <-> Room player UDP roundtrip through aliases");return 0;
}
static int delayed_mixed(int port) {
    prepare(port,"delayed-bridge");
    Config applied=g_config; applied.room_host=0; config_store_loaded(&applied); config_runtime_init(&applied);
    create(); assert(L.ryu && !mhgu_room_connected() && !mhgu_xb_active());
    Config changed=applied; snprintf(changed.room_server,sizeof changed.room_server,"127.0.0.1:%d",port+1);
    config_store_loaded(&changed); config_runtime_init(&changed);
    assert(!strcmp(room_keep_config.room_server,applied.room_server));
    char err[128]; assert(mhgu_roomsrv_start(port,"delayed-bridge","","delayed-bridge",err,sizeof err));
    uint64_t end=plat_now_ns()+20000000000ull;
    while(plat_now_ns()<end && !mhgu_xb_active()) Sleep(50);
    assert(mhgu_room_connected() && mhgu_xb_active());
    puts("PASS Mixed starts bridge after delayed Room join with originally applied settings");
    mhgu_roomsrv_stop();
    end=plat_now_ns()+10000000000ull;
    while(plat_now_ns()<end && (mhgu_room_connected() || mhgu_xb_active())) Sleep(50);
    assert(!mhgu_room_connected() && !mhgu_xb_active());
    assert(mhgu_roomsrv_start(port,"delayed-bridge","","delayed-bridge",err,sizeof err));
    end=plat_now_ns()+20000000000ull;
    while(plat_now_ns()<end && !mhgu_xb_active()) Sleep(50);
    assert(mhgu_room_connected() && mhgu_xb_active());
    puts("PASS Mixed reconnects and restores bridge using previously applied Room settings");
    destroy(); finish();return 0;
}
int main(int argc, char **argv) {
    setvbuf(stdout,NULL,_IONBF,0); assert(argc>=3); int port=atoi(argv[2]);
    if(argc>3 && !strcmp(argv[argc-1],"ryu")) transport=XP_RYU;
    if(argc>3 && !strcmp(argv[argc-1],"room")) transport=XP_ROOM;
    if(argc>3 && !strcmp(argv[argc-1],"mixed")) transport=XP_MIXED;
    emulator=transport!=XP_OFF;
    server_host=emulator && (!strcmp(argv[1],"host") || !strcmp(argv[1],"three-host") || !strcmp(argv[1],"delayed"));
    if(emulator) argc--;
    if(!strcmp(argv[1],"delayed")) return delayed_mixed(port);
    if(!strcmp(argv[1],"three-host")) return three_host(port,argv[3],argv[4]);
    if(!strcmp(argv[1],"three-peer")) return three_peer(port,argv[3]);
    if(!strcmp(argv[1],"edge")) return edge_cases(port);
    if(!strcmp(argv[1],"server")) return external_server(port,argv[3],argv[4]);
    if(!strcmp(argv[1],"remote")) return remote_host(port);
    if(!strcmp(argv[1],"lifecycle")) return lifecycle(port);
    if(!strcmp(argv[1],"host")) { assert(argc==5); return host(port,argv[3],argv[4]); }
    if(!strcmp(argv[1],"station")) { assert(argc==4); return station(port,argv[3]); }
    assert(!"unknown mode"); return 1;
}
