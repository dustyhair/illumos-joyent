#pragma D option quiet
BEGIN { printf("QUEUED_I386_ERRNO_READY\n"); }
fbt:lx_brand:lx_helper_rt_tgsigqueueinfo:entry
/execname == "queued_signal32"/
{
 this->si = (siginfo32_t *)copyin(arg3, sizeof (siginfo32_t));
 printf("HELPER32_INPUT errno=%d code=%d\n", this->si->si_errno, this->si->si_code);
}
fbt::sigaddqa:entry
/execname == "queued_signal32"/
{
 printf("QUEUED32_KERNEL errno=%d code=%d\n", ((sigqueue_t *)arg2)->sq_info.si_errno, ((sigqueue_t *)arg2)->sq_info.si_code);
}
fbt::siginfo_kto32:entry
/execname == "queued_signal32"/
{
 self->dest = arg1;
 printf("CONVERT32_INPUT errno=%d code=%d\n", ((k_siginfo_t *)arg0)->si_errno, ((k_siginfo_t *)arg0)->si_code);
}
fbt::siginfo_kto32:return
/self->dest != 0/
{
 printf("CONVERT32_OUTPUT errno=%d code=%d\n", ((siginfo32_t *)self->dest)->si_errno, ((siginfo32_t *)self->dest)->si_code);
 self->dest = 0;
}
