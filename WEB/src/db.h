#ifndef DB_H
#define DB_H

int db_init(const char *path);
int db_register_user(const char *name, const char *password);
int db_login_user(const char *name, const char *password);
int db_create_session(int user_id, char token[65]);
int db_session_user(const char *token);
void db_delete_session(const char *token);
int db_list_sensors(const char *filter, char *json, int capacity);
int db_save_sensor(int id, const char *name, const char *location,
                   double temperature, double humidity);
int db_delete_sensor(int id);
void db_close(void);

#endif
