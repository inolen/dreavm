#ifndef MICROPROFILE_IMPL_H
#define MICROPROFILE_IMPL_H

struct microprofile;
struct window;

struct microprofile *mp_create(struct window *window);
void mp_destroy(struct microprofile *mp);

void mp_begin_frame(struct microprofile *mp);
void mp_end_frame(struct microprofile *mp);

#endif
