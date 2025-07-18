#include "app_chameleon.h"
#include "chameleon_scene.h"

#include "mini_app_launcher.h"
#include "mini_app_registry.h"

#include "amiibo_helper.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"

#include "i18n/language.h"
#include "settings.h"

#include "mui_icons.h"
#include "tag_helper.h"

#include "fds_utils.h"

typedef enum {
    CHAMELEON_MENU_BACK,
    CHAMELEON_MENU_FILE,
    CHAMELEON_MENU_FOLDER,
    CHAMELEON_MENU_PARENT_DIR,
} chameleon_menu_item_t;

// Store the current browsing directory path
static char current_dir[VFS_MAX_PATH_LEN] = {0};

static int chameleon_scene_menu_card_data_list_item_cmp(const mui_list_item_t *p_item_a, const mui_list_item_t *p_item_b) {
    if (p_item_a->icon == ICON_HOME) {
        return -1;
    }
    if (p_item_b->icon == ICON_HOME) {
        return 1;
    }
    if (p_item_a->icon != p_item_b->icon) {
        return p_item_a->icon - p_item_b->icon;
    } else {
        return string_cmp(p_item_a->text, p_item_b->text);
    }
}


static void chameleon_scene_menu_card_data_file_load_from_file(app_chameleon_t *app, const char *file_name) {
    char path[VFS_MAX_PATH_LEN];
    vfs_file_t fd;
    vfs_obj_t obj;
    int32_t err;
    int32_t ret;

    // If current_dir is empty, use CHELEMEON_DUMP_FOLDER
    if (strlen(current_dir) == 0) {
        sprintf(path, "%s/%s", CHELEMEON_DUMP_FOLDER, file_name);
    } else {
        sprintf(path, "%s/%s", current_dir, file_name);
    }
    vfs_driver_t *p_driver = vfs_get_default_driver();

    tag_specific_type_t tag_type = tag_helper_get_active_tag_type();
    uint8_t *tag_buffer = tag_helper_get_active_tag_memory_data();
    size_t tag_data_size = tag_helper_get_active_tag_data_size();

    err = p_driver->stat_file(path, &obj);
    if (err < 0) {
        mui_toast_view_show(app->p_toast_view, _T(APP_CHAMELEON_CARD_DATA_LOAD_NOT_FOUND));
        return;
    }

    // TODO file size check
    if (tag_data_size != obj.size) {
        mui_toast_view_show(app->p_toast_view, _T(APP_CHAMELEON_CARD_DATA_LOAD_SIZE_NOT_MATCH));
        return;
    }

    // load file to buffer directly
    err = p_driver->read_file_data(path, tag_buffer, tag_data_size);
    if (err < 0) {
        mui_toast_view_show(app->p_toast_view, _T(APP_CHAMELEON_CARD_DATA_LOAD_FAILED));
        return;
    }

    // set nickname by filename
    err = tag_helper_set_nickname(file_name);
    if (err != 0) {
        mui_toast_view_show(app->p_toast_view, _T(APP_CHAMELEON_CARD_SET_NICK_FAILED));
        return;
    }

    NRF_LOG_INFO("load card data:%d", err);
    mui_toast_view_show(app->p_toast_view, _T(APP_CHAMELEON_CARD_DATA_LOAD_SUCCESS));
    mui_scene_dispatcher_back_scene(app->p_scene_dispatcher, 2);
}

static void chameleon_scene_menu_card_data_file_load_reload(app_chameleon_t *app) {
    vfs_dir_t dir;
    vfs_obj_t obj;
    char *browse_dir;

    // Clear the list, prepare to reload
    mui_list_view_clear_items(app->p_list_view);

    vfs_driver_t *p_driver = vfs_get_default_driver();
    
    // Determine the directory to browse
    if (strlen(current_dir) == 0) {
        browse_dir = CHELEMEON_DUMP_FOLDER;
    } else {
        browse_dir = current_dir;
    }
    
    int32_t res = p_driver->open_dir(browse_dir, &dir);
    if (res < 0) {
        // If failed to open directory, it might not exist, return to root directory
        strcpy(current_dir, "");
        mui_toast_view_show(app->p_toast_view, _T(OPEN_FOLDER_FAILED));
        return;
    }

    // If not in root directory, add ".." item for returning to parent directory
    if (strlen(current_dir) > 0) {
        mui_list_view_add_item(app->p_list_view, ICON_FOLDER, "..", (void *)CHAMELEON_MENU_PARENT_DIR);
    }

    // Traverse directory contents
    while (p_driver->read_dir(&dir, &obj) == VFS_OK) {
        if (obj.type == VFS_TYPE_DIR) {
            // If it's a directory, add to list with folder icon
            mui_list_view_add_item(app->p_list_view, ICON_FOLDER, obj.name, (void *)CHAMELEON_MENU_FOLDER);
        } else if (obj.type == VFS_TYPE_REG) {
            // If it's a file, add to list with file icon
            mui_list_view_add_item(app->p_list_view, ICON_FILE, obj.name, (void *)CHAMELEON_MENU_FILE);
        }
    }
    p_driver->close_dir(&dir);

    mui_list_view_sort(app->p_list_view, chameleon_scene_menu_card_data_list_item_cmp);
    
    // Add return button (ensure it's always at the end of the list)
    mui_list_view_add_item(app->p_list_view, ICON_BACK, _T(MAIN_RETURN), (void *)CHAMELEON_MENU_BACK);
    return;
}

// Helper function: Update current directory path
void update_current_directory(const char* new_dir) {
    if (strcmp(new_dir, "..") == 0) {
        // Return to parent directory
        char* last_slash = strrchr(current_dir, '/');
        if (last_slash) {
            *last_slash = '\0'; // Truncate string to remove the last directory level
            
            // If returning to the level above root directory, clear current_dir
            if (strlen(current_dir) == 0 || strcmp(current_dir, CHELEMEON_DUMP_FOLDER) == 0) {
                strcpy(current_dir, "");
            }
        } else {
            strcpy(current_dir, ""); // If no slash found, clear directly (return to root directory)
        }
    } else {
        // Enter subdirectory
        char temp[VFS_MAX_PATH_LEN];
        
        if (strlen(current_dir) == 0) {
            // If currently in root directory
            sprintf(temp, "%s/%s", CHELEMEON_DUMP_FOLDER, new_dir);
        } else {
            // If already in a subdirectory
            sprintf(temp, "%s/%s", current_dir, new_dir);
        }
        
        strcpy(current_dir, temp);
    }
}

static void chameleon_scene_menu_card_data_file_load_on_event(mui_list_view_event_t event, mui_list_view_t *p_list_view,
                                                       mui_list_item_t *p_item) {
    app_chameleon_t *app = p_list_view->user_data;
    chameleon_menu_item_t item = (chameleon_menu_item_t)p_item->user_data;
    
    switch (item) {
    case CHAMELEON_MENU_FILE: {
        chameleon_scene_menu_card_data_file_load_from_file(app, string_get_cstr(p_item->text));
    } break;
    
    case CHAMELEON_MENU_FOLDER: {
        // Enter subdirectory
        update_current_directory(string_get_cstr(p_item->text));
        chameleon_scene_menu_card_data_file_load_reload(app);
    } break;
    
    case CHAMELEON_MENU_PARENT_DIR: {
        // Return to parent directory
        update_current_directory("..");
        chameleon_scene_menu_card_data_file_load_reload(app);
    } break;

    case CHAMELEON_MENU_BACK:
        mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);
        break;
    }
}

void chameleon_scene_menu_card_data_file_load_on_enter(void *user_data) {
    app_chameleon_t *app = user_data;

    // Initialize current directory as empty (root directory)
    strcpy(current_dir, "");
    
    // Load file list (the reload function will now add a return button)
    chameleon_scene_menu_card_data_file_load_reload(app);
    
    mui_list_view_set_selected_cb(app->p_list_view, chameleon_scene_menu_card_data_file_load_on_event);
    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher, CHAMELEON_VIEW_ID_LIST);
}

void chameleon_scene_menu_card_data_file_load_on_exit(void *user_data) {
    app_chameleon_t *app = user_data;
    
    // Clean up list items
    mui_list_view_clear_items(app->p_list_view);
    
    // Clean up current directory path
    strcpy(current_dir, "");
}