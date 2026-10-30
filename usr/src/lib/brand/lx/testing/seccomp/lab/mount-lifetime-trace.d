sdt:lx_brand::lx-mntns-reap-retry
{
    @retry[arg0] = count();
}

END
{
    printa("RETIRE_RETRY errno=%d count=%@d\n", @retry);
}
