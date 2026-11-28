/* Runtime access and the separate journal for the Prowler wheel. */
static unsigned cat_enabled=1, cat_trigger, cat_opacity=3;
static unsigned cat_x=1113, cat_y=360, cat_scale=100;
static bool cat_position_edit;
static bool save_cat_options(unsigned enabled,unsigned trigger,unsigned opacity_value,
                             unsigned x,unsigned y,unsigned scale_percent);
