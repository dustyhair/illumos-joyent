/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
fbt:lx_ptm:lx_ptm_read:entry
/uid == 65534 && execname == "codex"/
{
    self->read_uio = arg1;
    self->read_before = ((struct uio *)arg1)->uio_resid;
    self->read_mode = ((struct uio *)arg1)->uio_fmode;
    self->read_dev = arg0;
}
fbt:lx_ptm:lx_ptm_read:return
/self->read_uio != 0/
{
    printf("PTY_READ pid=%d dev=%lx before=%ld after=%ld mode=%x ret=%d\n",
        pid, self->read_dev, self->read_before,
        ((struct uio *)self->read_uio)->uio_resid, self->read_mode, arg1);
    self->read_uio = 0;
}
fbt:lx_ptm:lx_ptm_poll:entry
/uid == 65534 && execname == "codex"/
{
    self->poll_events = arg1;
    self->poll_output = arg3;
}
fbt:lx_ptm:lx_ptm_poll:return
/self->poll_output != 0/
{
    printf("PTY_POLL pid=%d want=%x ready=%x ret=%d\n", pid,
        self->poll_events, *(short *)self->poll_output, arg1);
    self->poll_output = 0;
}
