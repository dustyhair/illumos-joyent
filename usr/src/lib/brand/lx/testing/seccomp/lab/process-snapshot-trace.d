fbt:lx_brand:lx_mntns_view_snapshot:entry
{
    self->snapshot = 1;
    printf("PROCESS_SNAPSHOT source=%s\n", stringof(arg0));
}

fbt:lx_brand:lx_mntns_view_bind:entry
/self->snapshot/
{
    self->bindpath = arg2;
    self->bindtype = ((struct vnode *)arg1)->v_type;
}

fbt:lx_brand:lx_mntns_view_bind:return
/self->bindpath/
{
    printf("PROCESS_BIND path=%s type=%d error=%d\n",
        stringof(self->bindpath), self->bindtype, arg1);
    self->bindpath = 0;
}

fbt:lx_brand:lx_mntns_view_snapshot:return
/self->snapshot/
{
    printf("PROCESS_SNAPSHOT_RETURN error=%d\n", arg1);
    self->snapshot = 0;
}
