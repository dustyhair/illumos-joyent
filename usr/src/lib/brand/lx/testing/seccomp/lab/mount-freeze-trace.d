fbt:lofs:lo_private_freeze:entry
{
    self->seal = 1;
    printf("SEAL vfs=%p flags=0x%x private=0x%x desired=0x%x\n", arg0,
        ((vfs_t *)arg0)->vfs_flag,
        ((struct loinfo *)((vfs_t *)arg0)->vfs_data)->li_flag,
        ((lofs_freeze_args_t *)((struct mounta *)arg1)->dataptr)->lfa_flags);
}
fbt:lofs:lo_private_freeze:return
/self->seal/
{
    printf("SEAL_RETURN errno=%d\n", (int)arg1);
    self->seal = 0;
}
