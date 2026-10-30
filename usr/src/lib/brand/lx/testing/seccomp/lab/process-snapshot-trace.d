fbt:lx_brand:lx_mntns_view_snapshot:entry
{
    self->snapshot = 1;
    self->construct = 0;
    printf("PROCESS_SNAPSHOT source=%s\n", stringof(arg0));
}

fbt:lx_brand:lx_mntns_bind:entry
/self->snapshot/
{
    self->bindpath = arg2;
    self->bindtype = ((struct vnode *)arg1)->v_type;
}

fbt:lx_brand:lx_mntns_bind:return
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

fbt:lx_brand:lx_mntns_mount:entry
/self->snapshot/
{
    self->mountpath = arg1;
    self->construct = 1;
    printf("PROCESS_MOUNT path=%s covered=%p type=%d flags=%x\n",
        stringof(arg1), arg5, ((struct vnode *)arg5)->v_type, arg4);
}

fbt:lx_brand:lx_mntns_lookup_at:entry
/self->snapshot && !self->construct/
{
    self->captureout = arg3;
    self->capturepath = arg1;
}

fbt:lx_brand:lx_mntns_lookup_at:return
/self->captureout/
{
    this->vp = *(struct vnode **)self->captureout;
    printf("PROCESS_CAPTURE path=%s error=%d type=%d\n",
        stringof(self->capturepath), arg1,
        this->vp != 0 ? this->vp->v_type : -1);
    self->captureout = 0;
}

fbt:lx_brand:lx_mntns_mount:return
/self->mountpath/
{
    printf("PROCESS_MOUNT_RETURN path=%s error=%d\n",
        stringof(self->mountpath), arg1);
    self->mountpath = 0;
}

fbt:lx_brand:lx_mntns_lookup_path:entry
/self->snapshot && stringof(arg1) == "/etc/mnttab"/
{
    self->lookupout = arg3;
    self->lookuproot = arg0;
}

fbt:lx_brand:lx_mntns_lookup_path:return
/self->lookupout/
{
    this->vp = *(struct vnode **)self->lookupout;
    printf("PROCESS_TARGET error=%d rootfs=%p target=%p type=%d fs=%p\n",
        arg1, ((struct vnode *)self->lookuproot)->v_vfsp, this->vp,
        this->vp->v_type, this->vp->v_vfsp);
    self->lookupout = 0;
}
