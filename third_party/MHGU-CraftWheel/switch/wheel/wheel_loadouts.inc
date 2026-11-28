/* Keep the selected preset and its item bindings in the same journal entry. */
typedef struct {
    uint32_t magic, seq, valid, active, targets[4][8], live[8], crc;
} LoadoutDisk;
static LoadoutDisk presets;
static const char *loadout_paths[] = {
    "savedata:/gu_wheel_loadouts_a.bin", "savedata:/gu_wheel_loadouts_b.bin"
};

static bool same_targets(const uint32_t *a, const uint32_t *b){
    for(unsigned i=0; i<8; i++)if(a[i]!=b[i])return false;
    return true;
}
static bool valid_targets(const uint32_t *items){
    for(unsigned i=0; i<8; i++)if(items[i]>=2991)return false;
    return true;
}
static bool read_loadout(const char *path, LoadoutDisk *d){
    if(!read_exact(path,d,sizeof(*d)) || d->magic!=0x314c5747 ||
       d->active>=4 || d->valid>15 || !(d->valid&(1u<<d->active)) || !valid_targets(d->live))
        return false;
    for(unsigned i=0;i<4;i++)if(!valid_targets(d->targets[i]))return false;
    return true;
}

static void import_presets(const Disk *live){
    PresetDisk a,b;
    bool va=read_presets(preset_paths[0],&a), vb=read_presets(preset_paths[1],&b);
    const PresetDisk *old=vb&&(!va||(int32_t)(b.seq-a.seq)>0)?&b:va?&a:0;
    memset(&presets,0,sizeof(presets));
    if(old){
        presets.valid=old->valid&15;
        copy(presets.targets,old->targets,sizeof(presets.targets));
    }
    unsigned first_saved=4, first_free=4, match=4;
    for(unsigned i=0;i<4;i++){
        if(presets.valid&(1u<<i)){
            if(first_saved==4)first_saved=i;
            if(live && same_targets(live->targets,presets.targets[i]) && match==4)match=i;
        }else if(first_free==4)first_free=i;
    }
    unsigned active=0;
    if(live){
        if(match<4)active=match;
        else if(first_free<4){
            active=first_free;
            copy(presets.targets[active],live->targets,sizeof(presets.live));
        }
        /* With four different saved presets, retain both the running wheel and
           the saved first preset until the player explicitly edits or recalls it. */
        copy(presets.live,live->targets,sizeof(presets.live));
    }else if(first_saved<4){
        active=first_saved;
        copy(presets.live,presets.targets[active],sizeof(presets.live));
    }
    presets.active=active;
    presets.valid|=1u<<active;
}
static void load_presets(const Disk *legacy_live){
    LoadoutDisk a,b;
    bool va=read_loadout(loadout_paths[0],&a), vb=read_loadout(loadout_paths[1],&b);
    const LoadoutDisk *record=vb&&(!va||(int32_t)(b.seq-a.seq)>0)?&b:va?&a:0;
    if(record)copy(&presets,record,sizeof(presets));
    else import_presets(legacy_live);
    copy(wheel.targets,presets.live,sizeof(presets.live));
}
static bool commit_loadout(LoadoutDisk *next){
    next->magic=0x314c5747;
    next->seq=presets.seq+1;
    next->crc=crc(next,sizeof(*next)-4);
    if(!storage_begin())return false;
    const char *path=loadout_paths[next->seq&1];
    LoadoutDisk check;
    bool ok=write_exact(path,next,sizeof(*next)) && read_loadout(path,&check) &&
            next->seq==check.seq && next->crc==check.crc;
    storage_end();
    if(ok)copy(&presets,next,sizeof(*next));
    return ok;
}
static bool save_preset(unsigned index, const uint32_t *items){
    if(index>=4)return false;
    LoadoutDisk next=presets;
    next.valid|=1u<<index;
    copy(next.targets[index],items,sizeof(next.live));
    if(index==next.active)copy(next.live,items,sizeof(next.live));
    return commit_loadout(&next);
}
static void select_preset_row(unsigned index){
    preset_row=index;
    previewing=index<4 && index!=presets.active;
    if(previewing){
        if(presets.valid&(1u<<index))copy(preview_targets,presets.targets[index],sizeof(preview_targets));
        else memset(preview_targets,0,sizeof(preview_targets));
    }
    status=index==5?"":previewing?"正在预览预设，按[RIGHT]呼出":"当前正在使用的轮盘";
}
static bool activate_preset(unsigned index){
    if(index>=4)return false;
    if(index!=presets.active || !same_targets(presets.live,presets.targets[index])){
        LoadoutDisk next=presets;
        next.active=index;
        if(!(next.valid&(1u<<index)))memset(next.targets[index],0,sizeof(next.live));
        next.valid|=1u<<index;
        copy(next.live,next.targets[index],sizeof(next.live));
        if(!commit_loadout(&next)){
            status="呼出保存失败，原绑定已保留";
            return false;
        }
    }
    copy(wheel.targets,presets.live,sizeof(presets.live));
    select_preset_row(index);
    ui_focus=0;
    status="预设已呼出";
    return true;
}
