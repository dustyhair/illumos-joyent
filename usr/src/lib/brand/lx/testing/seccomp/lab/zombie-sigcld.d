#pragma D option quiet

BEGIN
{
	printf("PID_ZOMBIE_TRACE_READY\n");
}

fbt::sigcld:entry
/execname == "pid_orphan" && arg1 == 0/
{
	self->zchild = ((proc_t *)arg0)->p_pid;
	self->zparent = ((proc_t *)arg0)->p_parent->p_pid;
	printf("PID_ZOMBIE_SIGCLD_NULL child=%d parent=%d\n",
	    self->zchild, self->zparent);
}

fbt::sigaddq:entry
/self->zchild != 0 && ((proc_t *)arg0)->p_pid == self->zparent/
{
	printf("PID_ZOMBIE_QUEUE_ALLOC child=%d parent=%d kmflag=%d\n",
	    self->zchild, self->zparent, arg3);
}

fbt::sigcld:return
/self->zchild != 0/
{
	self->zchild = 0;
	self->zparent = 0;
}
