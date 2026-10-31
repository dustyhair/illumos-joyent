#pragma D option quiet
fbt:lx_brand:lx_userns_child_create:entry
{
    self->out = arg1;
}
fbt:lx_brand:lx_userns_child_create:return
/self->out != 0 && arg1 == 0/
{
    ns = *(lx_brand`lx_userns_t **)self->out;
    printf("CREATE t=%u pid=%d ns=%p\n", timestamp, pid, ns);
    self->out = 0;
}
fbt:lx_brand:lx_userns_rele:entry
/arg0 != 0 && ((lx_brand`lx_userns_t *)arg0)->un_parent != 0/
{
    printf("RELE t=%u pid=%d ns=%p refs=%u\n", timestamp, pid, arg0,
        ((lx_brand`lx_userns_t *)arg0)->un_refs);
    stack(10);
}
sdt:lx_brand::lx-mntns-reap-retry
{
    printf("REAP_RETRY t=%u error=%d\n", timestamp, arg0);
}
