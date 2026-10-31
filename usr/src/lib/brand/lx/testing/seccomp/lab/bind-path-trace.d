#!/usr/sbin/dtrace -s
#pragma D option quiet
#pragma D option nolibs
fbt:lx_brand:lx_mntns_mount_bind:entry
{
    self->binding = 1;
    printf("BIND source=%s target=%s\n", stringof(arg1), stringof(arg2));
}
fbt::lookupname:entry
/self->binding/
{
    printf("LOOKUP path=%s\n", stringof(arg0));
}
fbt::lookupname:return
/self->binding/
{
    printf("LOOKUP_RESULT error=%d\n", arg1);
}
fbt::vnodetopath:entry
/self->binding/
{
    self->result_path = arg2;
    printf("PATH root=%p target=%p rootpath=%s targetpath=%s\n", arg0, arg1,
        arg0 == 0 ? "NULL" : stringof(((vnode_t *)arg0)->v_path),
        stringof(((vnode_t *)arg1)->v_path));
}
fbt::vnodetopath:return
/self->binding/
{
    printf("PATH_RESULT error=%d path=%s\n", arg1,
        arg1 == 0 ? stringof(self->result_path) : "FAILED");
    self->result_path = 0;
}
fbt::domount:return
/self->binding/
{
    printf("DOMOUNT_RESULT error=%d\n", arg1);
}
fbt:lx_brand:lx_mntns_bind_locked:return
/self->binding/
{
    printf("GRAFT_RESULT error=%d\n", arg1);
}
fbt:lx_brand:lx_mntns_mount_bind:return
/self->binding/
{
    printf("BIND_RESULT error=%d\n", arg1);
    self->binding = 0;
}
