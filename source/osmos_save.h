/* osmos_save.h -- local stand-ins for Play Games cloud save and achievements.
 * MIT licensed.
 *
 * CloudSaveHelper.getGameData()/saveGameData([B) are the only two calls the
 * engine makes for its cloud slot. Backing them with a local file keeps the
 * engine's own merge and restore paths coherent, which is less invasive than
 * convincing it there is no cloud at all.
 */
#ifndef OSMOS_SAVE_H
#define OSMOS_SAVE_H

void *osmos_save_read_cloud(int *len_out);   /* caller frees; NULL if absent */
void  osmos_save_write_cloud(const void *data, int len);
void  osmos_save_reset(void);
void  osmos_save_record_achievement(int id);

#endif
