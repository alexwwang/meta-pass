#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef void esp_event_handler_instance_t;
typedef int esp_event_base_t;
#define ESP_EVENT_ANY_BASE ((esp_event_base_t)-1)
#define ESP_EVENT_ANY_ID   ((int)-1)
static inline esp_err_t esp_event_handler_register(const esp_event_base_t base,
                                                    const int event_id,
                                                    void (*event_handler)(void *handler_arg,
                                                                          esp_event_base_t event_base,
                                                                          int event_id, void *event_data),
                                                    void *event_handler_arg) {
    (void)base;(void)event_id;(void)event_handler;(void)event_handler_arg;
    return 0;
}
static inline esp_err_t esp_event_handler_unregister(const esp_event_base_t base,
                                                      const int event_id,
                                                      void (*event_handler)(void *handler_arg,
                                                                            esp_event_base_t event_base,
                                                                            int event_id, void *event_data)) {
    (void)base;(void)event_id;(void)event_handler;
    return 0;
}
static inline esp_err_t esp_event_loop_create_default(void) { return 0; }
static inline void esp_event_loop_delete_default(void) {}
#ifdef __cplusplus
}
#endif
