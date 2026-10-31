#!/usr/sbin/dtrace -s
#pragma D option quiet
fbt:lx_brand:lx_mntns_mount_bind:entry
{
    self->binding = 1;
    printf("TYPE_BIND source=%s target=%s\n", stringof(arg1), stringof(arg2));
}
fbt::domount:entry
/self->binding/
{
    printf("TYPE_DOMOUNT type=%d flags=0x%x vfs=%p\n",
        ((vnode_t *)arg2)->v_type, ((vnode_t *)arg2)->v_flag,
        ((vnode_t *)arg2)->v_vfsp);
}
fbt:lofs:lo_mount:entry
/self->binding/
{
    printf("TYPE_LOFS_ENTER\n");
}
fbt::domount:return
/self->binding/
{
    printf("TYPE_DOMOUNT_RETURN error=%d\n", arg1);
}
fbt:lx_brand:lx_mntns_mount_bind:return
/self->binding/
{
    printf("TYPE_BIND_RETURN error=%d\n", arg1);
    self->binding = 0;
}
