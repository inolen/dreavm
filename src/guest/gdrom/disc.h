#ifndef DISC_H
#define DISC_H

#include "core/filesystem.h"
#include "guest/gdrom/gdrom_types.h"

#define DISC_MAX_SECTOR_SIZE 2352
#define DISC_MAX_SESSIONS 2
#define DISC_MAX_TRACKS 64
#define DISC_MAX_ID_SIZE 161

struct track {
  int num;
  /* frame adddress, equal to lba + 150 */
  int fad;
  /* type of information encoded in the sub q channel */
  int adr;
  /* type of track */
  int ctrl;
  /* sector info */
  int sector_fmt;
  int sector_size;
  int header_size;
  int error_size;
  int data_size;
  /* backing file */
  char filename[PATH_MAX];
  int frames;
  int extraframes;
  int file_offset;
  int chdofs;
  int phyofs;
};

struct session {
  int leadin_fad;
  int leadout_fad;
  int first_track;
  int last_track;
};

struct disc {
  char id[DISC_MAX_ID_SIZE];
  int bootfad;
  int bootlen;

  void (*destroy)(struct disc *);

  int (*get_format)(struct disc *);

  int (*get_num_sessions)(struct disc *);
  struct session *(*get_session)(struct disc *, int);

  int (*get_num_tracks)(struct disc *);
  struct track *(*get_track)(struct disc *, int);

  void (*get_toc)(struct disc *, int, struct track **, struct track **, int *,
                  int *);
  void (*read_sector)(struct disc *, struct track *, int, void *);
};

struct disc *disc_create(const char *filename);
void disc_destroy(struct disc *disc);

int disc_get_format(struct disc *disc);
int disc_get_num_sessions(struct disc *disc);
struct session *disc_get_session(struct disc *disc, int n);
int disc_get_num_tracks(struct disc *disc);
struct track *disc_get_track(struct disc *disc, int n);
struct track *disc_lookup_track(struct disc *disc, int fad);
void disc_get_toc(struct disc *disc, int area, struct track **first_track,
                  struct track **last_track, int *leadin_fad, int *leadout_fad);

int disc_find_file(struct disc *disc, const char *filename, int *fad, int *len);
int disc_read_sectors(struct disc *disc, int fad, int num_sectors,
                      int sector_fmt, int sector_mask, void *dst, int dst_size);
int disc_read_bytes(struct disc *disc, int fad, int len, void *dst,
                    int dst_size);

#endif
