#include "prismafs.h"

// multiple base layers can be combined for session view in single mount
char base_paths[MAX_BASE_LAYERS][PATH_MAX];
int  num_base_layers = 0;

char session_path[PATH_MAX]; // session layer

// helper func to construct full path in the session layer
void session_fullpath(char fpath[PATH_MAX], const char *path)
{
    // snprintf builds string into a buffer
    if (session_path[strlen(session_path) - 1] == '/' && path[0] == '/')
        // PATH_MAX avoids buffer overflow
        // path + 1 skips leading "/" on path to avoid double slash //
        snprintf(fpath, PATH_MAX, "%s%s", session_path, path + 1);
    else
        snprintf(fpath, PATH_MAX, "%s%s", session_path, path);
}

// walks through all base layers in order and builds the full path to the file.
// returns 0 and fills fpath if the file is found in any base layer, -1 if not found in any.
int base_fullpath_func(char fpath[PATH_MAX], const char *path) {
    for (int i = 0; i < num_base_layers; i++) {
        // avoid double slash on joining base and file path
        if (base_paths[i][strlen(base_paths[i]) - 1] == '/' && path[0] == '/')
            snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path + 1);
        else
            snprintf(fpath, PATH_MAX, "%s%s", base_paths[i], path);

        // check if the file actually exists at this location
        if (access(fpath, F_OK) == 0) {
            return 0; // found it, fpath is now set to the real location
        }
    }
    return -1; // not found in any base layer
}

// helper func for checking if filename is in linked list
int is_in_list(struct filename_node *filename_list, const char *name) {
    struct filename_node *current = filename_list;

    // walking linked list
    // [node1] -> [node2] -> [node3] -> NULL
    //  "foo"      "bar"      "baz"
    /*
    current starts at first node - strcmp compares node name to what its looking for (return 0 when strings are equal)
    if match then return 1, if no match then move to the next node
    when current is at the end of list (NULL), exit loop, func returns 0 (not found)
    */
    while (current != NULL) {
        if (strcmp(current->name, name) == 0)
            return 1;
        current = current->next;
    }
    return 0;
}

// helper to add filename to linked list
void add_to_list(struct filename_node **filename_list_ptr, const char *name) {
    // memory chunk for filename_node struct, malloc returns pointer to allocated memory, if NULL then allocation failed
    struct filename_node *new_node = malloc(sizeof(struct filename_node));
    // safeguard check just in case malloc fails
    if (new_node == NULL) {
        perror("malloc");
        return;
    }

    // make owned copy of filename string for this node to keep
    // if that copy failed, free the node and return
    new_node->name = strdup(name);
    if (new_node->name == NULL) {
        perror("strdup");
        free(new_node);
        return;
    }

    new_node->next = *filename_list_ptr;
    *filename_list_ptr = new_node;
}
