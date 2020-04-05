#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/rbtree.h>
#include <linux/uaccess.h>

MODULE_LICENSE("GPL");


struct phb_name_node
{
    struct rb_node node;
    char *name;
    char *phone_number;
    char *email;
};

struct phb_surname_node
{
    struct rb_node node;
    struct rb_root *name_tree_root;
    char *surname;
};

static struct rb_root surname_tree_root = RB_ROOT;

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);

static struct file_operations f_ops = {
    .read = device_read,
    .write = device_write,
    .open = device_open,
    .release = device_release,
};


static char out_idle[] = "Idle\n";
static char out_invalid_operation[] = "[Error.1] Requested operation is invalid\n";
static char out_argument_too_long[] = "[Error.2] Argument provided is too long\n";
static char out_specify_surname[] = "[Error.3] You have to specify surname\n";
static char out_specify_name[] = "[Error.4] You have to specify name\n";
static char out_surname_not_found[] = "[Error.5] Surname not found\n";
static char out_name_not_found[] = "[Error.6] Name not found\n";
static char out_deletion_successful[] = "[OK.1] Entries successfully deleted\n";
static char out_insertion_successful[] = "[OK.2] Entry was successfully inserted\n";
static char out_row_separator[] = "\n";
static char out_surname_prefix[] = "Surname:\t";
static char out_name_prefix[] = "Name:\t\t";
static char out_email_prefix[] = "Email:\t\t";
static char out_phone_number_prefix[] = "Phone number:\t";

static bool is_dynamic_state = false;
static bool eof_reached = false;
static char *state = out_idle;
static char device_open_count = 0;
static int major_num;

static void change_state_to_constant(char *to){
    if(is_dynamic_state)
        kfree(state);
    is_dynamic_state = false;
    state = to;
}

static void change_state_to_dynamic(char *to){
    if(is_dynamic_state)
        kfree(state);
    is_dynamic_state = true;
    state = to;
}

struct row_list_node{
    struct row_list_node *next;
    char *string;
    char size;
};

static struct phb_surname_node *surname_search(char *surname){
    struct rb_node *node = surname_tree_root.rb_node;
    while(node){
        struct phb_surname_node *surname_node = container_of(node, struct phb_surname_node, node);
        int cmp = strcmp(surname, surname_node->surname);
        if(cmp){
            if(cmp > 0)
                node = node->rb_right;
            else
                node = node->rb_left;
        }
        else{
            return surname_node;
        }
    }
    return NULL;
}

static struct phb_name_node *name_search(char *name, struct rb_root *name_tree_root){
    struct rb_node *node = name_tree_root->rb_node;
    while(node){
        struct phb_name_node *name_node = container_of(node, struct phb_name_node, node);
        int cmp = strcmp(name, name_node->name);
        if(cmp){
            if(cmp > 0)
                node = node->rb_right;
            else
                node = node->rb_left;
        }
        else{
            return name_node;
        }
    }
    return NULL;
}

static char *allocate_string(char length){
    char *result = kmalloc_array(length + 1, sizeof(char), GFP_KERNEL);
    *result = length;
    return ++result;
}

static void free_string(char *firstSymbol){
    if(firstSymbol)
        kfree(--firstSymbol);
}

static struct phb_surname_node *insert_surname_node_at(struct rb_node **at, struct rb_node *parent, char *surname){
    struct phb_surname_node *surname_node = kmalloc(sizeof(struct phb_surname_node), GFP_KERNEL);
    surname_node->name_tree_root = kmalloc(sizeof(struct rb_root), GFP_KERNEL);
    *(surname_node->name_tree_root) = RB_ROOT;
    surname_node->surname = surname;
    rb_link_node(&(surname_node->node), parent, at);
    rb_insert_color(&(surname_node->node), &(surname_tree_root));
    return surname_node;
}

static struct phb_name_node *insert_name_node_at(struct rb_node **at, struct rb_node *parent, struct rb_root *name_tree, char *name){
    struct phb_name_node *name_node = kmalloc(sizeof(struct phb_name_node), GFP_KERNEL);
    name_node->email = NULL;
    name_node->phone_number = NULL;
    name_node->name = name;
    rb_link_node(&(name_node->node), parent, at);
    rb_insert_color(&(name_node->node), name_tree);
    return name_node;
}


static struct phb_surname_node *get_surname_node(char *surname){
    struct rb_node **cur = &(surname_tree_root.rb_node);
    struct rb_node *parent = NULL;
    while(*cur) {
        struct phb_surname_node *cur_snode = container_of(*cur, struct phb_surname_node, node);
        int cmp = strcmp(surname, cur_snode->surname);
        parent = *cur;
        if(cmp){
            if(cmp > 0)
                cur = &((*cur)->rb_right);
            else
                cur = &((*cur)->rb_left);
        }
        else
        {
            return cur_snode;
        }
    }

    return insert_surname_node_at(cur, parent, surname);
}

static struct phb_name_node *get_name_node(struct rb_root *name_tree, char *name){
    struct rb_node **cur = &(name_tree->rb_node), *parent = NULL;
    while(*cur) {
        struct phb_name_node *cur_nnode = container_of(*cur, struct phb_name_node, node);
        int cmp = strcmp(name, cur_nnode->name);
        parent = *cur;
        if(cmp){
            if(cmp > 0)
                cur = &((*cur)->rb_right);
            else
                cur = &((*cur)->rb_left);
        }
        else
        {
            return cur_nnode;
        }
    }
    return insert_name_node_at(cur, parent, name_tree, name);
}

static void free_name_node(struct phb_name_node *name_node)
{
    free_string(name_node->name);
    free_string(name_node->email);
    free_string(name_node->phone_number);
    kfree(name_node);
}

static void free_surname_node(struct phb_surname_node *surname_node)
{
    kfree(surname_node->name_tree_root);
    free_string(surname_node->surname);
    kfree(surname_node);
}

static int delete_user(char *name, char *surname){
    struct phb_surname_node *surname_node = surname_search(surname);
    struct phb_name_node *name_node;
    if(!surname_node)
        return -1;
    name_node = name_search(name, surname_node->name_tree_root);
    if(!name_node)
        return -2;
    rb_erase(&(name_node->node), surname_node->name_tree_root);
    free_name_node(name_node);
    if(!(surname_node->name_tree_root->rb_node)){
        rb_erase(&(surname_node->node), &(surname_tree_root));
        free_surname_node(surname_node);
    }
    return 0;
}

static void delete_name_subtree(struct rb_node *node){
    if(node){
        delete_name_subtree(node->rb_right);
        delete_name_subtree(node->rb_left);
        free_name_node(container_of(node, struct phb_name_node, node));
    }
}

static void delete_surname_subtree(struct rb_node *node){
    if(node){
        struct phb_surname_node *surname_node = container_of(node, struct phb_surname_node, node);
        delete_surname_subtree(node->rb_right);
        delete_surname_subtree(node->rb_left);
        delete_name_subtree(surname_node->name_tree_root->rb_node);
        free_surname_node(surname_node);
    }
}

static int delete_users(char *surname){
    struct phb_surname_node *surname_node = surname_search(surname);
    if(!surname_node)
        return -1;

    delete_name_subtree(surname_node->name_tree_root->rb_node);
    rb_erase(&(surname_node->node), &(surname_tree_root));
    free_surname_node(surname_node);
    return 0;
}

static int delete_all_users(void){
    delete_surname_subtree(surname_tree_root.rb_node);
    surname_tree_root = RB_ROOT;
    return 0;
}

static struct row_list_node *append_string(struct row_list_node *parent, char *string, size_t *counter){
    struct row_list_node *child = kmalloc(sizeof(struct row_list_node), GFP_KERNEL);
    child->string = string;
    child->next = NULL;
    child->size = string[-1];
    parent->next = child;
    (*counter) = (*counter) + child->size - 1;
    return child;
}

static struct row_list_node *append_carray(struct row_list_node *parent, char carray[], char size, size_t *counter){
    struct row_list_node *child = kmalloc(sizeof(struct row_list_node), GFP_KERNEL);
    child->string = carray;
    child->next = NULL;
    child->size = size;
    parent->next = child;
    (*counter) = (*counter) + child->size - 1;
    return child;
}

// chars_count withought terminating 0
static char *build_string(struct row_list_node *first, size_t chars_count){
    char *result = kmalloc_array(chars_count + 1, sizeof(char), GFP_KERNEL);
    char *cur_char = result;
    struct row_list_node *parent = NULL;
    struct row_list_node *cur;
    for(cur = first; cur; cur = cur->next)
    {
        kfree(parent);
        strcpy(cur_char, cur->string);
        cur_char += cur->size - 1;
        parent = cur;
    }
    kfree(parent);
    return result;
}

static struct row_list_node *append_user_to_list(char *surname, struct phb_name_node *name_node,
                                          struct row_list_node *to, size_t *counter){
    struct row_list_node *cur = to;
    cur = append_carray(cur, out_surname_prefix, ARRAY_SIZE(out_surname_prefix), counter);
    cur = append_string(cur, surname, counter);
    cur = append_carray(cur, out_row_separator, ARRAY_SIZE(out_row_separator), counter);

    cur = append_carray(cur, out_name_prefix, ARRAY_SIZE(out_name_prefix), counter);
    cur = append_string(cur, name_node->name, counter);
    cur = append_carray(cur, out_row_separator, ARRAY_SIZE(out_row_separator), counter);

    if(name_node->phone_number != NULL){
        cur = append_carray(cur, out_phone_number_prefix, ARRAY_SIZE(out_phone_number_prefix), counter);
        cur = append_string(cur, name_node->phone_number, counter);
        cur = append_carray(cur, out_row_separator, ARRAY_SIZE(out_row_separator), counter);
    }
    if(name_node->email != NULL){
        cur = append_carray(cur, out_email_prefix, ARRAY_SIZE(out_email_prefix), counter);
        cur = append_string(cur, name_node->email, counter);
        cur = append_carray(cur, out_row_separator, ARRAY_SIZE(out_row_separator), counter);
    }
    return cur;
}

static char *get_user_string(char *surname, struct phb_name_node *name_node){
    struct row_list_node empty;
    size_t counter = 0;
    append_user_to_list(surname, name_node, &(empty), &(counter));
    return build_string(empty.next, counter);
}

static char *get_users_string(struct phb_surname_node *surname_node){
    struct row_list_node empty, *head = &(empty);
    size_t counter = 0;
    struct rb_node *cur = rb_first(surname_node->name_tree_root);
    if(cur){
        head = append_user_to_list(surname_node->surname, container_of(cur, struct phb_name_node, node),
                                   head, &(counter));
        cur = rb_next(cur);
    }
    for(; cur; cur = rb_next(cur)){
        head = append_carray(head, out_row_separator, ARRAY_SIZE(out_row_separator), &(counter));
        head = append_user_to_list(surname_node->surname, container_of(cur, struct phb_name_node, node),
                                   head, &(counter));
    }
    return build_string(empty.next, counter);
}

static void free_dw_str_memory(char *name, char *surname, char *email, char *phone_number){
    free_string(name);
    free_string(surname);
    free_string(email);
    free_string(phone_number);
}

static void do_get_operation(char *surname, char *name){
    struct phb_surname_node *surname_node;
    if(!surname){
        change_state_to_constant(out_specify_surname);
        return;
    }
    surname_node = surname_search(surname);
    if(!surname_node){
        change_state_to_constant(out_surname_not_found);
        return;
    }
    if(name){
        struct phb_name_node *name_node = name_search(name, surname_node->name_tree_root);
        if(!name_node){
            change_state_to_constant(out_name_not_found);
            return;
        }
        change_state_to_dynamic(get_user_string(surname, name_node));
    }
    else
        change_state_to_dynamic(get_users_string(surname_node));
}

static void do_delete_operation(char *surname, char *name){
    int result = 1;
    if(surname){
        if(name)
            result = delete_user(name, surname);
        else
            result = delete_users(surname);
    }
    else{
        result = delete_all_users();
    }
    if(result == 0)
        change_state_to_constant(out_deletion_successful);
    else if(result == -1)
        change_state_to_constant(out_surname_not_found);
    else
        change_state_to_constant(out_name_not_found);
}

static void do_insert_operation(char *surname, char *name, char *email, char *phone_number){
    struct phb_surname_node *surname_node;
    struct phb_name_node *name_node;
    if(!surname){
        change_state_to_constant(out_specify_surname);
        free_dw_str_memory(surname, name, email, phone_number);
        return;
    }
    if(!name){
        change_state_to_constant(out_specify_name);
        free_dw_str_memory(surname, name, email, phone_number);
        return;
    }
    surname_node = get_surname_node(surname);
    name_node = get_name_node(surname_node->name_tree_root, name);
    if(surname_node->surname != surname){
        free_string(surname);
    }
    if(name_node->name != name)
        free_string(name);

    if(email){
        free_string(name_node->email);
        name_node->email = email;
    }
    if(phone_number){
        free_string(name_node->phone_number);
        name_node->phone_number = phone_number;
    }
    change_state_to_constant(out_insertion_successful);
}

static ssize_t device_read(struct file *flip, char *buffer, size_t len, loff_t *offset){
    ssize_t bytes_read = 0;
    char *cur = state;

    if(eof_reached)
    {
        eof_reached = false;
        return 0;
    }

    for(; len && *cur; cur++, buffer++, bytes_read++, len--)
        put_user(*cur, buffer);

    if(len && (*cur == 0)){
        put_user(0, buffer);
	eof_reached = true;
	bytes_read++;
    }
    return bytes_read;
}

/*
* -g to get data
* -i to isert data
* -d to delete data
* -s to set surname
* -n to set name
* -e to set email
* -t to set phone number
*/
static ssize_t device_write(struct file *file, const char *buffer, size_t len, loff_t *offset){
    const char max_arg_length = 64;
    const char idle = 0;
    const char arg_option_type = 1;
    const char arg_option_value = 2;
    const char arg_option_separator = 3;
    char write_state = idle;
    char next_state = idle;
    char operation_type = 0;

    char *name = NULL;
    char *surname = NULL;
    char *email = NULL;
    char *phone_number = NULL;
    char **cur_arg = NULL;
    char cur;

    size_t arg_length = 0;
    size_t i = 0;

    // begin read supplied arguments
    if(i < len)
        get_user(cur, buffer); // tchartdev -i simename -s surname -t +8883324 -e email@none.com

    for(; i < len && cur; i++){
        if(write_state == idle){
            if(cur == '-'){
                write_state = arg_option_type;
            }
            else if(!(cur == ' ' || cur == '\n')){
                change_state_to_constant(out_invalid_operation);
                free_dw_str_memory(name, surname, email, phone_number);
                return -EINVAL;
            }
        }
        else if(write_state == arg_option_type)
        {
            if(cur == 'g' || cur == 'i' || cur == 'd'){
                operation_type = cur;
                write_state = idle;
            }
            else{
                switch(cur){
                case 'n':
                    cur_arg = &(name);
                    break;
                case 's':
                    cur_arg = &(surname);
                    break;
                case 'e':
                    cur_arg = &(email);
                    break;
                case 't':
                    cur_arg = &(phone_number);
                    break;
                default:
                    free_dw_str_memory(name, surname, email, phone_number);
                    return -EINVAL;
                }
                write_state = arg_option_separator;
                next_state = arg_option_value;
            }
        }
        else if(write_state == arg_option_separator){
            if(!(cur == ' ' || cur == '\n')){
                i--;
                write_state = next_state;
                next_state = idle;
            }
        }
        else if(write_state == arg_option_value){
            if(cur == ' ' || cur == '\n'){
		free_string(*cur_arg);
                (*cur_arg) = allocate_string(arg_length + 1);
                (*cur_arg)[arg_length] = 0;
                copy_from_user((*cur_arg), buffer + i - arg_length, arg_length);
                write_state = idle;
                arg_length = 0;
            }
            else{
                arg_length++;
                if(arg_length > max_arg_length){
                    change_state_to_constant(out_argument_too_long);
                    free_dw_str_memory(name, surname, email, phone_number);
                    return -EINVAL;
                }
            }
        }
        else{
            change_state_to_constant(out_invalid_operation);
            free_dw_str_memory(name, surname, email, phone_number);
            return -EINVAL;
        }

        if(i + 1 < len){
            get_user(cur, buffer + i + 1);
        }
    }
    // finalise reading for states that need this
    if(write_state == arg_option_value){
	free_string(*cur_arg);
        (*cur_arg) = allocate_string(arg_length + 1);
        (*cur_arg)[arg_length] = 0;
        copy_from_user((*cur_arg), buffer + i - arg_length, arg_length);
        write_state = idle;
        arg_length = 0;
    }

    if(i < len && cur == 0)
        i++;
    // end read supplied arguments

    if(operation_type == 'g'){
        do_get_operation(surname, name);
        free_dw_str_memory(name, surname, email, phone_number);
    }
    else if(operation_type == 'i'){
        do_insert_operation(surname, name, email, phone_number);
    }
    else if(operation_type == 'd'){
        do_delete_operation(surname, name);
        free_dw_str_memory(name, surname, email, phone_number);
    }
    else{
        change_state_to_constant(out_invalid_operation);
        free_dw_str_memory(name, surname, email, phone_number);
        return -EINVAL;
    }

    return i;
}

static int device_open(struct inode *inode, struct file *file){
    if(device_open_count)
        return -EBUSY;

    device_open_count++;
    try_module_get(THIS_MODULE);
    return 0;
}

static int device_release(struct inode *inode, struct file *file){
    device_open_count--;
    module_put(THIS_MODULE);
    return 0;
}

static int __init tchardev_module_init(void){
    major_num = register_chrdev(0, "tchardev", &f_ops);
    if(major_num < 0){
        printk(KERN_ALERT "Could not register device: %d\n", major_num);
        return major_num;
    }
    else {
        printk(KERN_INFO "tchardev device major number %d\n", major_num);
	return 0;
    }
}


static void __exit tchardev_module_exit(void){
    unregister_chrdev(major_num, "tchardev");
    delete_all_users();
    if(is_dynamic_state)
        kfree(state);
}

module_init(tchardev_module_init);
module_exit(tchardev_module_exit);
