#pragma D option quiet

BEGIN
{
 printf("QUEUED_ZERO_TRACE_READY\n");
}
fbt:lx_brand:lx_helper_rt_tgsigqueueinfo:entry
{
 self->zero = arg2 == 0;
 @calls[arg2 == 0] = count();
}
fbt::kmem_zalloc:entry
/self->zero && arg0 == sizeof (sigqueue_t)/
{
 zero_queue_alloc_seen = 1;
 @zero_zalloc_calls = count();
 @zero_zalloc_bytes = sum(arg0);
}
fbt:lx_brand:lx_helper_rt_tgsigqueueinfo:return
{
 self->zero = 0;
}

END
{
 printf("QUEUED_ZERO_ALLOC_SEEN=%d\n", zero_queue_alloc_seen);
 printa("QUEUED_HELPER_CALLS zero=%d count=%@d\n", @calls);
}
