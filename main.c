/*
  Pundra CSE Smart Portal 25
  C Backend + SQLite3 + HTML/CSS frontend, no JavaScript.
  Target platform: Windows MSYS2 UCRT64.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <sqlite3.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET socket_t;
  #define CLOSESOCKET closesocket
#else
  #include <unistd.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  typedef int socket_t;
  #define INVALID_SOCKET -1
  #define SOCKET_ERROR -1
  #define CLOSESOCKET close
#endif

#define DEFAULT_PORT 8080
static int g_port = DEFAULT_PORT;
#define DB_FILE "pundra_cse_smart_portal_25.db"
#define BUF_RECV 65536
#define MAX_BODY 262144

static sqlite3 *g_db = NULL;

/* ------------------------- basic dynamic string ------------------------- */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} String;

static void s_init(String *s) {
    s->cap = 8192;
    s->len = 0;
    s->data = (char*)malloc(s->cap);
    if (!s->data) { fprintf(stderr, "Out of memory\n"); exit(1); }
    s->data[0] = '\0';
}

static void s_reserve(String *s, size_t extra) {
    if (s->len + extra + 1 <= s->cap) return;
    while (s->len + extra + 1 > s->cap) s->cap *= 2;
    char *p = (char*)realloc(s->data, s->cap);
    if (!p) { fprintf(stderr, "Out of memory\n"); exit(1); }
    s->data = p;
}

static void s_append(String *s, const char *text) {
    size_t n = strlen(text);
    s_reserve(s, n);
    memcpy(s->data + s->len, text, n + 1);
    s->len += n;
}

static void s_appendf(String *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) { va_end(ap2); return; }
    s_reserve(s, (size_t)needed);
    vsnprintf(s->data + s->len, s->cap - s->len, fmt, ap2);
    va_end(ap2);
    s->len += (size_t)needed;
}

static char *s_take(String *s) { return s->data; }

/* ------------------------- helpers ------------------------- */
static const char *safe_text(sqlite3_stmt *st, int col) {
    const unsigned char *t = sqlite3_column_text(st, col);
    return t ? (const char*)t : "";
}

static void html_escape_to(String *out, const char *src) {
    if (!src) return;
    for (const char *p = src; *p; p++) {
        switch (*p) {
            case '&': s_append(out, "&amp;"); break;
            case '<': s_append(out, "&lt;"); break;
            case '>': s_append(out, "&gt;"); break;
            case '"': s_append(out, "&quot;"); break;
            case '\'': s_append(out, "&#39;"); break;
            default: {
                char c[2] = {*p, 0}; s_append(out, c);
            }
        }
    }
}

static char *html_escape(const char *src) {
    String s; s_init(&s); html_escape_to(&s, src); return s_take(&s);
}

static void now_string(char *buf, size_t size) {
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", tmv);
}

static void today_string(char *buf, size_t size) {
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);
    strftime(buf, size, "%Y-%m-%d", tmv);
}

static unsigned long long fnv1a_hash(const char *text) {
    unsigned long long h = 1469598103934665603ULL;
    const char *salt = "PUCSE25_SECURE_LOCAL_SALT";
    for (const unsigned char *p = (const unsigned char*)salt; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
    for (const unsigned char *p = (const unsigned char*)text; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
    return h;
}

static void password_hash_hex(const char *password, char out[32]) {
    snprintf(out, 32, "%016llx", fnv1a_hash(password ? password : ""));
}

static int db_exec(const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\nSQL: %s\n", err ? err : "unknown", sql);
        sqlite3_free(err);
        return 0;
    }
    return 1;
}

static int scalar_int(const char *sql) {
    sqlite3_stmt *st = NULL;
    int value = 0;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        value = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return value;
}

static double scalar_double(const char *sql) {
    sqlite3_stmt *st = NULL;
    double value = 0;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        value = sqlite3_column_double(st, 0);
    }
    sqlite3_finalize(st);
    return value;
}

static double grade_point_from_mark(double marks) {
    if (marks >= 80) return 4.00;
    if (marks >= 75) return 3.75;
    if (marks >= 70) return 3.50;
    if (marks >= 65) return 3.25;
    if (marks >= 60) return 3.00;
    if (marks >= 55) return 2.75;
    if (marks >= 50) return 2.50;
    if (marks >= 45) return 2.25;
    if (marks >= 40) return 2.00;
    return 0.00;
}

static const char *letter_from_mark(double marks) {
    if (marks >= 80) return "A+";
    if (marks >= 75) return "A";
    if (marks >= 70) return "A-";
    if (marks >= 65) return "B+";
    if (marks >= 60) return "B";
    if (marks >= 55) return "B-";
    if (marks >= 50) return "C+";
    if (marks >= 45) return "C";
    if (marks >= 40) return "D";
    return "F";
}

static void log_activity(const char *actor, const char *action, const char *details) {
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO activity_logs(actor, action, details, created_at) VALUES(?,?,?,datetime('now','localtime'))";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, actor ? actor : "system", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, action ? action : "update", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, details ? details : "", -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

static void add_mail_log(const char *recipient, const char *subject, const char *message, const char *status) {
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO mail_logs(recipient, subject, message, status, created_at) VALUES(?,?,?,?,datetime('now','localtime'))";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, recipient ? recipient : "batch25@local", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, subject ? subject : "Portal Update", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, message ? message : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, status ? status : "logged", -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

/* ------------------------- database schema and seed ------------------------- */
static void create_schema(void) {
    db_exec("PRAGMA foreign_keys = ON;");
    db_exec("CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY AUTOINCREMENT, full_name TEXT, email TEXT UNIQUE, password_hash TEXT, role TEXT, department TEXT, batch TEXT, semester TEXT, status TEXT DEFAULT 'Active', created_at TEXT DEFAULT CURRENT_TIMESTAMP);");
    db_exec("CREATE TABLE IF NOT EXISTS departments(id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE, summary TEXT, vision TEXT, mission TEXT, source_url TEXT, updated_at TEXT DEFAULT CURRENT_TIMESTAMP);");
    db_exec("CREATE TABLE IF NOT EXISTS teachers(id INTEGER PRIMARY KEY AUTOINCREMENT, full_name TEXT UNIQUE, short_form TEXT, designation TEXT, mobile TEXT, email TEXT, department TEXT, photo_url TEXT, status TEXT DEFAULT 'Active');");
    db_exec("CREATE TABLE IF NOT EXISTS students(id INTEGER PRIMARY KEY AUTOINCREMENT, roll_no TEXT, student_id TEXT UNIQUE, full_name TEXT, department TEXT, batch TEXT, semester TEXT, phone TEXT, email TEXT, address TEXT, status TEXT DEFAULT 'Active');");
    db_exec("CREATE TABLE IF NOT EXISTS courses(id INTEGER PRIMARY KEY AUTOINCREMENT, course_code TEXT UNIQUE, course_title TEXT, credit REAL, course_type TEXT, department TEXT, batch TEXT, semester TEXT, teacher_short TEXT, teacher_name TEXT, room TEXT, status TEXT DEFAULT 'Active');");
    db_exec("CREATE TABLE IF NOT EXISTS attendance(id INTEGER PRIMARY KEY AUTOINCREMENT, date TEXT, course_code TEXT, student_id TEXT, status TEXT, note TEXT, updated_by TEXT, created_at TEXT DEFAULT CURRENT_TIMESTAMP, UNIQUE(date, course_code, student_id));");
    db_exec("CREATE TABLE IF NOT EXISTS ct_exams(id INTEGER PRIMARY KEY AUTOINCREMENT, course_code TEXT, title TEXT, exam_type TEXT, exam_date TEXT, full_mark REAL, note TEXT, status TEXT DEFAULT 'Scheduled');");
    db_exec("CREATE TABLE IF NOT EXISTS notices(id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT, course_code TEXT, notice_from TEXT, message TEXT, priority TEXT, publish_date TEXT, status TEXT DEFAULT 'Active');");
    db_exec("CREATE TABLE IF NOT EXISTS groups_table(id INTEGER PRIMARY KEY AUTOINCREMENT, group_name TEXT UNIQUE, focus_area TEXT, description TEXT, leader TEXT, meeting_time TEXT, status TEXT DEFAULT 'Open');");
    db_exec("CREATE TABLE IF NOT EXISTS group_members(id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, student_id TEXT, student_name TEXT, reason TEXT, status TEXT DEFAULT 'Pending', created_at TEXT DEFAULT CURRENT_TIMESTAMP);");
    db_exec("CREATE TABLE IF NOT EXISTS library_books(id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT UNIQUE, author TEXT, category TEXT, total_copy INTEGER, available_copy INTEGER, shelf TEXT);");
    db_exec("CREATE TABLE IF NOT EXISTS library_issues(id INTEGER PRIMARY KEY AUTOINCREMENT, book_id INTEGER, student_id TEXT, student_name TEXT, issue_date TEXT, due_date TEXT, return_date TEXT, status TEXT DEFAULT 'Issued');");
    db_exec("CREATE TABLE IF NOT EXISTS canteen_items(id INTEGER PRIMARY KEY AUTOINCREMENT, item_name TEXT UNIQUE, price REAL, category TEXT, status TEXT DEFAULT 'Available');");
    db_exec("CREATE TABLE IF NOT EXISTS canteen_orders(id INTEGER PRIMARY KEY AUTOINCREMENT, customer_name TEXT, item_name TEXT, quantity INTEGER, unit_price REAL, total REAL, order_date TEXT DEFAULT CURRENT_TIMESTAMP);");
    db_exec("CREATE TABLE IF NOT EXISTS blood_donors(id INTEGER PRIMARY KEY AUTOINCREMENT, student_id TEXT UNIQUE, full_name TEXT, blood_group TEXT, phone TEXT, last_donation TEXT, availability TEXT, note TEXT);");
    db_exec("CREATE TABLE IF NOT EXISTS labs(id INTEGER PRIMARY KEY AUTOINCREMENT, lab_name TEXT UNIQUE, room_no TEXT, pc_count INTEGER, available_pc INTEGER, lab_assistant TEXT, status TEXT, details TEXT);");
    db_exec("CREATE TABLE IF NOT EXISTS student_reports(id INTEGER PRIMARY KEY AUTOINCREMENT, student_id TEXT, student_name TEXT, report_type TEXT, details TEXT, status TEXT DEFAULT 'Open', created_at TEXT DEFAULT CURRENT_TIMESTAMP);");
    db_exec("CREATE TABLE IF NOT EXISTS mail_logs(id INTEGER PRIMARY KEY AUTOINCREMENT, recipient TEXT, subject TEXT, message TEXT, status TEXT, created_at TEXT DEFAULT CURRENT_TIMESTAMP);");
    db_exec("CREATE TABLE IF NOT EXISTS activity_logs(id INTEGER PRIMARY KEY AUTOINCREMENT, actor TEXT, action TEXT, details TEXT, created_at TEXT DEFAULT CURRENT_TIMESTAMP);");
    db_exec("CREATE TABLE IF NOT EXISTS settings(id INTEGER PRIMARY KEY AUTOINCREMENT, key TEXT UNIQUE, value TEXT);");
    db_exec("CREATE TABLE IF NOT EXISTS department_images(id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT UNIQUE, description TEXT, image_url TEXT, source_url TEXT);");
    db_exec("CREATE TABLE IF NOT EXISTS routine_slots(id INTEGER PRIMARY KEY AUTOINCREMENT, day_name TEXT, time_slot TEXT, course_code TEXT, teacher_short TEXT, room_no TEXT, note TEXT, UNIQUE(day_name,time_slot,course_code));");
    db_exec("CREATE TABLE IF NOT EXISTS routine_assets(id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT UNIQUE, image_url TEXT, note TEXT, updated_at TEXT DEFAULT CURRENT_TIMESTAMP);");
    db_exec("CREATE TABLE IF NOT EXISTS result_records(id INTEGER PRIMARY KEY AUTOINCREMENT, student_id TEXT, student_name TEXT, course_code TEXT, course_title TEXT, credit REAL, marks REAL, letter_grade TEXT, grade_point REAL, published_by TEXT, publish_date TEXT DEFAULT CURRENT_TIMESTAMP, UNIQUE(student_id, course_code));");
    db_exec("DELETE FROM blood_donors WHERE id NOT IN (SELECT MIN(id) FROM blood_donors GROUP BY student_id);");
    db_exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_blood_donors_student_id ON blood_donors(student_id);");
}

typedef struct { const char *roll; const char *sid; const char *name; } StudentSeed;
static StudentSeed STUDENTS[] = {
{"01","0322310105101008","Md. Tanvir Hamid"},{"02","0322410105101002","Mohibbullah"},{"03","03224205101005","Sahara Mahajabin"},{"04","03224205101009","Ratna Rani"},{"05","03224205101015","Md. Hanjala Sumon"},{"06","03224205101016","Mst. Surya Tajin"},{"07","03224205101017","Mst. Ferdusi Jannat"},{"08","03224205101024","Mst. Israth Jahan Mysha"},{"09","03224205101026","Md. Jakirul Islam"},{"10","03224205101029","Anamika Binte Anowar"},{"11","03224205101031","Mst. Mushrat Jahan"},{"12","03224205101033","Md. Shabab Alam Deep"},{"13","03224205101034","Md. Saim Sarkar"},{"14","03224205101035","Mst. Mohua Akter"},{"15","03224205101036","Mantasha Mamun Mahi"},{"16","03224205101039","Nur-E-Jannat"},{"17","03224205101041","Sabbir Hasan"},{"18","03224205101043","Md. Farhan Al Shahriar Siam"},{"19","03224205101045","Md. Khalid Almas Soumik"},{"20","03224205101046","Mst. Tahasina Arzuman Toru"},{"21","03224205101047","Luhana Rashid Himi"},{"22","03224205101048","Md. Mehedi Hasan Sagor"},{"23","03224205101051","Md. Nasib Ali"},{"24","03224205101053","Shirajum Monira Maysha"},{"25","03224205101054","Chandon Kumar Sarker"},{"26","03224205101055","Md. Shahabus Sadik Sadi"},{"27","03224205101056","Md. Reshad Shahriar"},{"28","03224205101057","Mst. Mahim Ebade"},{"29","03224205101058","Md. Shariful Islam"},{"30","03224205101059","Md. Feroz Ahmed"},{"31","03224205101060","Shahinur Ahammed"},{"32","03224205101063","Mst. Suraiya Yeasmin"},{"33","03224205101064","Shamiul Islam Shanto"},{"34","03224205101066","Md. Sohanur Islam Shafollo"},{"35","03224205101067","Md. Abdul Alim"},{"36","03224205101068","Md. Naim Babu"},{"37","03224205101070","Md. Nazmul Hossain"},{"38","03224205101071","Kazi Humaira"},{NULL,NULL,NULL}
};

typedef struct { const char *name,*shortf,*designation,*mobile,*email,*photo; } TeacherSeed;
static TeacherSeed TEACHERS[] = {
{"Md. Habib Ehsanul Hoque","HEH","Asst. Professor & Head","01786-044388","ehsanamil@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2Fmd-habib-ehsanul-hoque.png&w=640"},
{"Moslema Jahan","MJ","Asst. Professor","01731-557230","moslemajahan.math@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2Fmoslema-jahan.png&w=640"},
{"Indraneel Misra","IM","Asst. Professor","01792-047999","misraindraneel@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2F20260314140302_IMG_1348_1-removebg-preview.png&w=640"},
{"Most. Rehena Khatun","MRK","Asst. Professor","01571-786959","rehenak1991@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2Fmost-rehena-khatun.png&w=640"},
{"Mrittika Mahbub","MM","Lecturer","01701-577906","mrittikatania@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2Fmrittika-mahbub.png&w=640"},
{"Md. Ataur Rahman","AR","Lecturer (on Study Leave)","","","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2Fmd-ataur-rahman.png&w=640"},
{"Md. Riadul Islam Chowdhury","RIC","Lecturer","01822-821083","riadul.mric@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2Fmd-riadul-islam-chowdhury.png&w=640"},
{"Radha Rani Paul","RRP","Lecturer","01623-895008","radhapaul03@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2Fradha-rani-paul.png&w=640"},
{"Iffath Tanjim Moon","ITM","Lecturer","01738-063082","iffathtanjim2197@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2Fiffath-tanjim-moon.png&w=640"},
{"Mst. Sahela Rahman","SR","Lecturer","01780-784770","srtinni@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2Fmst-sahela-rahman.png&w=640"},
{"Nahid Hasan","NH","Lecturer","01521-543374","nahidpundra.cse@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2Fnahid-hasan.png&w=640"},
{"M. Z. I. Juwel","MZI","Lecturer","01303-283604","mzijuwel@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2FM._Z._I._Juwel.png&w=640"},
{"Md. Bipul Islam","MBI","Lecturer","01576-536139","bipulice18106@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2FMd._Bipul_Islam.png&w=640"},
{"Md. Forhan Shahriar Fahim","FSF","Lecturer","01318-486103","forhan.shahriar.fahim@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2FMd._Forhan_Shahriar_Fahim.png&w=640"},
{"Nafiu Rahman","NR","Lecturer","01632-201717","nafiur463@gmail.com","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2F20260418100417_Nafiu_Rahman_300-removebg-preview.png&w=640"},
{"Dr. Dipankar Das","DD","Professor (Adjunct Faculty)","","dipankar@ru.ac.bd","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2FDr._Dipankar_Das.png&w=640"},
{"Dr. Aurangzib Md. Abdur Rahman","AMR","Professor (Adjunct Faculty)","","amar_ice@ru.ac.bd","https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2FDr._Aurangzib_Md._Abdur_Rahman_CSE.png&w=640"},
{NULL,NULL,NULL,NULL,NULL,NULL}
};

typedef struct { const char *code,*title; double credit; const char *type,*teacher_short,*teacher,*room; } CourseSeed;
static CourseSeed COURSES[] = {
{"CSE 3100","Software Development Project I",1.0,"Project","MRK","Mst. Rehena Khatun","NB-502"},
{"CSE 2103","Database Management System",3.0,"Theory","MBI","Md. Bipul Islam","NB-504"},
{"CSE 2104","Database Management System Sessional",1.0,"Sessional","MBI","Md. Bipul Islam","NB-406"},
{"CSE 2101","Design and Analysis of Algorithm",3.0,"Theory","FSF","Md. Forhan Shahriar Fahim","NB-503"},
{"CSE 2102","Design and Analysis of Algorithm Sessional",1.0,"Sessional","FSF","Md. Forhan Shahriar Fahim","NB-407"},
{"CSE 2203","Data Communication",3.0,"Theory","IM","Indraneel Misra","NB-503"},
{"CSE 2207","Numerical Methods",2.0,"Theory","MJ","Moslema Jahan","NB-504"},
{"MTH 2201","Complex Variable, Probability and Statistics",3.0,"Theory","MJ","Moslema Jahan","NB-508"},
{"HUM 2201","Bangladesh Studies and History of Independence",2.0,"Theory","MR","Masud Rana","NB-503"},
{"HUM 3101","Professional Ethics and Environmental Protection",2.0,"Theory","IAZ","Imran Ali Zehadi","NB-508"},
{NULL,NULL,0,NULL,NULL,NULL,NULL}
};

static void seed_database(void) {
    if (scalar_int("SELECT COUNT(*) FROM users") == 0) {
        char hash[32]; password_hash_hex("Pucse@2025", hash);
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(g_db, "INSERT INTO users(full_name,email,password_hash,role,department,batch,semester,status) VALUES(?,?,?,?,?,?,?,?)", -1, &st, NULL);
        sqlite3_bind_text(st,1,"System Administrator",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,2,"admin@pucse25.local",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,3,hash,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,4,"Super Admin",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,5,"Computer Science & Engineering",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,6,"25",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,7,"4th",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,8,"Active",-1,SQLITE_TRANSIENT);
        sqlite3_step(st); sqlite3_finalize(st);
    }
    db_exec("INSERT OR IGNORE INTO departments(name,summary,vision,mission,source_url) VALUES('Computer Science & Engineering','The Department of Computer Science & Engineering of Pundra University focuses on high standard engineering education, modern classrooms and laboratories, research, workshops, seminars, academic contests and extra-curricular activities.','Student learning is the top priority. The department aims to develop technology skills, individual talents and critical thinking while working toward excellence in engineering and technology education and research.','Provide high quality CSE education, produce competent engineers, support research and innovation, strengthen industry interaction, and build leadership, ethics and lifelong learning attitude.','https://pundrauniversity.ac.bd/departments/computer-science-and-engineering')");

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO teachers(full_name,short_form,designation,mobile,email,department,photo_url,status) VALUES(?,?,?,?,?,?,?,?)", -1, &st, NULL);
    for (int i=0; TEACHERS[i].name; i++) {
        sqlite3_reset(st); sqlite3_clear_bindings(st);
        sqlite3_bind_text(st,1,TEACHERS[i].name,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,2,TEACHERS[i].shortf,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,3,TEACHERS[i].designation,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,4,TEACHERS[i].mobile,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,5,TEACHERS[i].email,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,6,"Computer Science & Engineering",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,7,TEACHERS[i].photo,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,8,"Active",-1,SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO students(roll_no,student_id,full_name,department,batch,semester,phone,email,address,status) VALUES(?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL);
    for (int i=0; STUDENTS[i].name; i++) {
        sqlite3_reset(st); sqlite3_clear_bindings(st);
        sqlite3_bind_text(st,1,STUDENTS[i].roll,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,2,STUDENTS[i].sid,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,3,STUDENTS[i].name,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,4,"Computer Science & Engineering",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,5,"25",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,6,"4th Semester",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,7,"Update required",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,8,"",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,9,"",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,10,"Active",-1,SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO courses(course_code,course_title,credit,course_type,department,batch,semester,teacher_short,teacher_name,room,status) VALUES(?,?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL);
    for (int i=0; COURSES[i].code; i++) {
        sqlite3_reset(st); sqlite3_clear_bindings(st);
        sqlite3_bind_text(st,1,COURSES[i].code,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,2,COURSES[i].title,-1,SQLITE_TRANSIENT);
        sqlite3_bind_double(st,3,COURSES[i].credit);
        sqlite3_bind_text(st,4,COURSES[i].type,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,5,"Computer Science & Engineering",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,6,"25",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,7,"4th Semester",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,8,COURSES[i].teacher_short,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,9,COURSES[i].teacher,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,10,COURSES[i].room,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,11,"Active",-1,SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    db_exec("INSERT OR IGNORE INTO ct_exams(course_code,title,exam_type,exam_date,full_mark,note,status) VALUES"
            "('CSE 3100','Lab Final','Lab Final','2026-04-16',50,'Software Development Project I lab final.','Scheduled'),"
            "('CSE 2103','CT 1','Class Test','2026-04-20',20,'DBMS basics, ER model and relational model.','Scheduled'),"
            "('CSE 2104','Lab Report Submission','Lab Report','2026-04-22',10,'DBMS sessional lab report.','Scheduled'),"
            "('CSE 2101','CT 1','Class Test','2026-04-24',20,'Algorithm complexity and divide and conquer.','Scheduled'),"
            "('CSE 2102','Lab Assignment','Lab Assignment','2026-04-27',15,'Algorithm sessional problem solving assignment.','Scheduled'),"
            "('CSE 2203','CT','Class Test','2026-04-29',20,'Data communication fundamentals.','Scheduled'),"
            "('CSE 2207','Class Test','Class Test','2026-05-03',20,'Numerical methods initial chapters.','Scheduled')");

    db_exec("INSERT OR IGNORE INTO notices(title,course_code,notice_from,message,priority,publish_date,status) VALUES"
            "('Department Update','ALL','Department','CSE 25 Batch portal dashboard has been updated with current student, course, attendance, notice, library, lab and report modules.','High',date('now'),'Active'),"
            "('Software Project Lab Final','CSE 3100','Course Teacher','Software Development Project I lab final is scheduled on 16 April. Bring project files and report.','High','2026-04-10','Active'),"
            "('DBMS Class On','CSE 2103','Course Teacher','DBMS class will continue according to routine. Students must bring notebook.','Medium',date('now'),'Active'),"
            "('Algorithm Assignment','CSE 2102','CR','Algorithm sessional assignment submission deadline has been added to CT & Exams page.','Medium',date('now'),'Active'),"
            "('Library Reminder','ALL','Librarian','Return issued CSE books within due date to avoid fine.','Low',date('now'),'Active')");

    db_exec("INSERT OR IGNORE INTO groups_table(group_name,focus_area,description,leader,meeting_time,status) VALUES"
            "('Data Research Group','Research','Student research group for data analysis, AI, machine learning and paper reading.','Md. Shariful Islam','Sunday 3:30 PM','Open'),"
            "('Website Project Group','Web Project','HTML CSS C backend project practice group for department portal and university software projects.','Md. Tanvir Hamid','Monday 2:00 PM','Open'),"
            "('Robotics Group','Robotics','Microcontroller, sensors, automation and robotics practice group.','Sabbir Hasan','Tuesday 3:45 PM','Open'),"
            "('Software Development Group','Software Engineering','Project planning, database design, report writing and deployment practice.','Sahara Mahajabin','Wednesday 2:30 PM','Open'),"
            "('Programming Practice Group','Programming','C, C++, SQL and algorithm practice for lab and contest preparation.','Mohibbullah','Saturday 3:00 PM','Open')");

    db_exec("INSERT OR IGNORE INTO department_images(title,description,image_url,source_url) VALUES"
            "('CSE Department Head','Official CSE faculty profile photo from Pundra University website.','https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2Fmd-habib-ehsanul-hoque.png&w=640','https://pundrauniversity.ac.bd/departments/computer-science-and-engineering/faculty-members'),"
            "('CSE Faculty Member','Official CSE faculty profile photo from Pundra University website.','https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2Fmoslema-jahan.png&w=640','https://pundrauniversity.ac.bd/departments/computer-science-and-engineering/faculty-members'),"
            "('CSE Academic Team','Official CSE faculty profile photo used for department gallery card.','https://pundrauniversity.ac.bd/_next/image?q=75&url=https%3A%2F%2Fadmin.pundrauniversity.ac.bd%2Fpublic%2Fassets%2Fimages%2Femployee%2Fmd-riadul-islam-chowdhury.png&w=640','https://pundrauniversity.ac.bd/departments/computer-science-and-engineering/faculty-members')");

    db_exec("INSERT OR IGNORE INTO routine_assets(title,image_url,note) VALUES('CSE 25 Batch Default Routine','/images/cse25_routine.svg','Default routine recreated from the uploaded class routine for CSE 25 Batch, 4th Semester. You can change this image path from the Routine page.')");

    db_exec("INSERT OR IGNORE INTO routine_slots(day_name,time_slot,course_code,teacher_short,room_no,note) VALUES"
            "('Saturday','09:30 AM - 10:45 AM','CSE 2101','FSF','NB-503','Design and Analysis of Algorithm'),"
            "('Saturday','10:45 AM - 12:00 PM','CSE 3100','MRK','NB-407','Software Development Project I'),"
            "('Saturday','12:00 PM - 01:15 PM','CSE 2207','MJ','NB-508','Numerical Methods'),"
            "('Sunday','09:30 AM - 10:45 AM','CSE 2101','FSF','NB-503','Design and Analysis of Algorithm'),"
            "('Sunday','10:45 AM - 12:00 PM','HUM 3101','IAZ','NB-503','Professional Ethics and Environmental Protection'),"
            "('Sunday','12:00 PM - 01:15 PM','MTH 2201','MJ','NB-503','Complex Variable, Probability and Statistics'),"
            "('Sunday','02:30 PM - 03:45 PM','HUM 2201','MR','NB-503','Bangladesh Studies and History of Independence'),"
            "('Monday','09:30 AM - 10:45 AM','CSE 2103','MBI','NB-503','Database Management System'),"
            "('Monday','10:45 AM - 12:00 PM','CSE 2102','FSF','NB-407','Design and Analysis of Algorithm Sessional'),"
            "('Monday','12:00 PM - 01:15 PM','MTH 2201','MJ','NB-502','Complex Variable, Probability and Statistics'),"
            "('Monday','02:30 PM - 03:45 PM','CSE 2203','IM','NB-504','Data Communication'),"
            "('Tuesday','09:30 AM - 10:45 AM','CSE 2103','MBI','NB-504','Database Management System'),"
            "('Tuesday','10:45 AM - 12:00 PM','CSE 2104','MBI','NB-406','Database Management System Sessional'),"
            "('Tuesday','12:00 PM - 01:15 PM','CSE 2203','IM','NB-503','Data Communication'),"
            "('Tuesday','02:30 PM - 03:45 PM','CSE 3100','MRK','NB-407','Project/Lab practice')");

    db_exec("INSERT OR IGNORE INTO library_books(title,author,category,total_copy,available_copy,shelf) VALUES"
            "('Introduction to Algorithms','Cormen, Leiserson, Rivest, Stein','Algorithm',5,5,'CSE-A1'),"
            "('Database System Concepts','Silberschatz, Korth, Sudarshan','Database',6,6,'CSE-B1'),"
            "('Computer Networks','Andrew S. Tanenbaum','Networking',4,4,'CSE-C1'),"
            "('Operating System Concepts','Silberschatz, Galvin, Gagne','Operating System',5,5,'CSE-D1'),"
            "('Let Us C','Yashavant Kanetkar','Programming',8,8,'CSE-P1'),"
            "('Clean Code','Robert C. Martin','Software Engineering',3,3,'CSE-S1'),"
            "('Artificial Intelligence: A Modern Approach','Russell and Norvig','AI',3,3,'CSE-AI'),"
            "('Discrete Mathematics and Its Applications','Kenneth H. Rosen','Mathematics',4,4,'CSE-M1'),"
            "('Numerical Methods','E. Balagurusamy','Numerical Methods',5,5,'CSE-N1'),"
            "('Software Engineering','Ian Sommerville','Software Engineering',4,4,'CSE-S2')");

    db_exec("INSERT OR IGNORE INTO canteen_items(item_name,price,category,status) VALUES"
            "('Tea',10,'Drinks','Available'),('Coffee',25,'Drinks','Available'),('Singara',10,'Snacks','Available'),('Samosa',10,'Snacks','Available'),('Paratha',15,'Breakfast','Available'),('Egg',20,'Breakfast','Available'),('Khichuri',50,'Meal','Available'),('Chicken Roll',60,'Snacks','Available'),('Rice Meal',90,'Meal','Available'),('Water Bottle',20,'Drinks','Available')");

    const char *bloods[] = {"A+","B+","O+","AB+","A-","B-","O+","A+"};
    sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO blood_donors(student_id,full_name,blood_group,phone,last_donation,availability,note) VALUES(?,?,?,?,?,?,?)", -1, &st, NULL);
    for (int i=0; STUDENTS[i].name; i++) {
        sqlite3_reset(st); sqlite3_clear_bindings(st);
        sqlite3_bind_text(st,1,STUDENTS[i].sid,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,2,STUDENTS[i].name,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,3,bloods[i%8],-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,4,"Update required",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,5,(i%3==0)?"6 months ago":((i%3==1)?"4 months ago":"Not donated yet"),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,6,(i%4==0)?"Available":"Need confirmation",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(st,7,"Default donor profile. Update phone and last donation from admin panel.",-1,SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    db_exec("INSERT OR IGNORE INTO labs(lab_name,room_no,pc_count,available_pc,lab_assistant,status,details) VALUES"
            "('Computer Lab NB-408','NB-408',35,31,'AKM Ahsan Kabir','Active','Routine-based computer lab for CSE classes.'),"
            "('Computer Lab NB-503','NB-503',40,36,'Md. Sabbir Alam','Active','Software and database lab room.'),"
            "('Computer Lab NB-504','NB-504',36,32,'AKM Ahsan Kabir','Active','Programming and algorithm practice lab.'),"
            "('Computer Lab NB-508','NB-508',40,38,'Md. Ziaul Haque','Active','Course and project demonstration room.'),"
            "('Computer Lab NB-702','NB-702',32,30,'Mst. Parul Akter','Active','High floor CSE lab room from routine.'),"
            "('Computer Lab NB-703','NB-703',32,29,'Md. Ziaul Haque','Maintenance','Some PCs need software update.'),"
            "('Microprocessor Lab','Main Building',25,22,'Md. Sabbir Alam','Active','Microprocessor, embedded system and hardware practice.'),"
            "('Networking Lab','Main Building',20,18,'AKM Ahsan Kabir','Active','Router, switch and network cable practice.'),"
            "('Project Lab','Main Building',24,23,'Md. Ziaul Haque','Active','Final project and software development practice room.')");

    db_exec("INSERT OR IGNORE INTO settings(key,value) VALUES"
            "('smtp_mode','log_only'),('smtp_host',''),('smtp_port','587'),('smtp_user',''),('smtp_password',''),('theme','blue-purple'),('portal_name','Pundra CSE Smart Portal 25')");

    if (scalar_int("SELECT COUNT(*) FROM activity_logs") == 0) {
        log_activity("system", "Database seeded", "Default CSE 25 Batch data, courses, notices, labs and library records loaded.");
        add_mail_log("batch25@local", "Portal initialized", "Initial portal data has been created. SMTP mode is log only.", "logged");
    }
}

/* ------------------------- request parser ------------------------- */
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

static char *url_decode(const char *src) {
    size_t n = strlen(src);
    char *out = (char*)malloc(n + 1);
    char *q = out;
    for (size_t i = 0; i < n; i++) {
        if (src[i] == '+') *q++ = ' ';
        else if (src[i] == '%' && i + 2 < n && isxdigit((unsigned char)src[i+1]) && isxdigit((unsigned char)src[i+2])) {
            *q++ = (char)((hexval(src[i+1]) << 4) | hexval(src[i+2]));
            i += 2;
        } else *q++ = src[i];
    }
    *q = '\0';
    return out;
}

static char *form_value(const char *body, const char *key) {
    if (!body || !key) return strdup("");
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if ((p == body || *(p-1) == '&') && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            const char *e = strchr(p, '&');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            char *raw = (char*)malloc(len + 1);
            memcpy(raw, p, len); raw[len] = '\0';
            char *dec = url_decode(raw);
            free(raw);
            return dec;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return strdup("");
}

static int is_logged_in(const char *headers) {
    return headers && strstr(headers, "Cookie:") && strstr(headers, "pucse_session=active");
}

/* ------------------------- http response ------------------------- */
static void send_raw(socket_t client, const char *status, const char *ctype, const char *body, const char *extra_headers) {
    char header[2048];
    int len = body ? (int)strlen(body) : 0;
    snprintf(header, sizeof(header),
             "HTTP/1.1 %s\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n%s\r\n",
             status, ctype, len, extra_headers ? extra_headers : "");
    send(client, header, (int)strlen(header), 0);
    if (body && len > 0) send(client, body, len, 0);
}

static void redirect_to(socket_t client, const char *location, const char *extra) {
    char h[1024];
    snprintf(h, sizeof(h), "Location: %s\r\n%s", location, extra ? extra : "");
    send_raw(client, "302 Found", "text/plain", "Redirecting", h);
}

static char *load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char*)malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);
    return buf;
}

/* ------------------------- page components ------------------------- */
static void page_head(String *s, const char *title, int autorefresh) {
    s_appendf(s, "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>%s<title>%s</title><link rel='stylesheet' href='/style.css'></head><body>",
        autorefresh ? "<meta http-equiv='refresh' content='30'>" : "", title);
}

static void page_end(String *s) {
    s_append(s, "</body></html>");
}

static void sidebar(String *s, const char *active) {
    const char *items[][2] = {
        {"/dashboard","Dashboard"},{"/department","Department"},{"/teachers","Teachers"},{"/students","Students"},{"/courses","Courses"},{"/routine","Routine"},{"/attendance","Attendance"},{"/ct-exams","CT & Exams"},{"/results","Result Publish"},{"/notices","Notices"},{"/groups","Groups"},{"/library","Library"},{"/canteen","Canteen"},{"/blood-donation","Blood Donation"},{"/labs","Lab Management"},{"/reports","Student Reports"},{"/activity","Activity"},{"/settings","Settings"},{"/password","Password"},{"/logout","Logout"},{NULL,NULL}
    };
    s_append(s, "<aside class='sidebar'><div class='brand'><div class='logo'>PU</div><div><b>Pundra CSE</b><span>Smart Portal 25</span></div></div><nav>");
    for (int i=0; items[i][0]; i++) {
        const char *cls = (active && strcmp(active, items[i][1]) == 0) ? " class='active'" : "";
        s_appendf(s, "<a%s href='%s'>%s</a>", cls, items[i][0], items[i][1]);
    }
    s_append(s, "</nav></aside>");
}

static void layout_start(String *s, const char *title, const char *active, const char *subtitle, int autorefresh) {
    page_head(s, title, autorefresh);
    s_append(s, "<div class='app'>");
    sidebar(s, active);
    s_append(s, "<main class='main'>");
    s_appendf(s, "<section class='topbar'><div><h1>%s</h1><p>%s</p></div><div class='badge'>CSE 25 Batch, 4th Semester</div></section>", title, subtitle ? subtitle : "");
}

static void layout_end(String *s) {
    s_append(s, "</main></div>");
    page_end(s);
}

static void db_table(String *s, const char *sql, const char **headers, int ncols) {
    sqlite3_stmt *st = NULL;
    s_append(s, "<div class='table-wrap'><table><thead><tr>");
    for (int i=0; i<ncols; i++) s_appendf(s, "<th>%s</th>", headers[i]);
    s_append(s, "</tr></thead><tbody>");
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            s_append(s, "<tr>");
            for (int i=0; i<ncols; i++) {
                char *e = html_escape(safe_text(st, i));
                s_appendf(s, "<td>%s</td>", e);
                free(e);
            }
            s_append(s, "</tr>");
        }
    }
    sqlite3_finalize(st);
    s_append(s, "</tbody></table></div>");
}

/* ------------------------- pages ------------------------- */
static char *page_login(const char *error) {
    String s; s_init(&s); page_head(&s, "Login - Pundra CSE Smart Portal 25", 0);
    s_append(&s, "<div class='auth-bg'><div class='auth-shell'><section class='auth-info'><div class='mini-label'>Pundra University</div><h1>Department of Computer Science & Engineering</h1><p class='lead'>A dedicated academic portal for CSE 25 Batch to manage courses, attendance, notices, teachers, library, lab, reports and departmental updates from one secure system.</p><div class='auth-stats'><span>25 Batch</span><span>4th Semester</span><span>CSE Portal</span></div></section><section class='auth-card login-card'><div class='auth-mark'>Secure Login</div><h2>Pundra CSE Smart Portal 25</h2><p>Use your authorized account to continue. Default admin information is not displayed on this page.</p>");
    if (error && *error) s_appendf(&s, "<div class='alert danger'>%s</div>", error);
    s_append(&s, "<form method='post' action='/login' class='form'><label>Email Address</label><input type='email' name='email' required autocomplete='username' placeholder='Enter your email'><label>Password</label><div class='password-wrap'><input id='login_password' type='password' name='password' required autocomplete='current-password' placeholder='Enter your password'><button class='show-pass' type='button' onclick='var p=this.previousElementSibling;p.type=p.type==`password`?`text`:`password`;this.innerText=p.type==`password`?`Show`:`Hide`;'>Show</button></div><button type='submit'>Login to Portal</button></form><div class='auth-links'><a href='/register'>Create student registration</a></div><p class='small muted center-text'>CSE 25 Batch, 4th Semester portal access. For access problems, contact the department admin.</p></section></div></div>");
    page_end(&s); return s_take(&s);
}

static char *page_register(const char *msg) {
    String s; s_init(&s); page_head(&s, "Registration - Pundra CSE Smart Portal 25", 0);
    s_append(&s, "<div class='auth-bg'><div class='auth-shell register-shell'><section class='auth-info'><div class='mini-label'>Student Registration</div><h1>CSE 25 Batch Registration</h1><p class='lead'>Submit your student information for instant CSE 25 Batch, 4th Semester portal access. No admin approval is required.</p><div class='auth-stats'><span>CSE</span><span>25 Batch</span><span>Instant Access</span></div></section><section class='auth-card wide'><div class='auth-mark'>Registration</div><h2>Create Portal Account</h2><p>New registration will be active immediately after submission.</p>");
    if (msg && *msg) s_appendf(&s, "<div class='alert ok'>%s</div>", msg);
    s_append(&s, "<form method='post' action='/register' class='form grid2'><div><label>Full Name</label><input name='full_name' required placeholder='Enter full name'></div><div><label>Email</label><input type='email' name='email' required placeholder='Enter email'></div><div><label>Password</label><div class='password-wrap'><input id='reg_password' type='password' name='password' required placeholder='Create password'><button class='show-pass' type='button' onclick='var p=this.previousElementSibling;p.type=p.type==`password`?`text`:`password`;this.innerText=p.type==`password`?`Show`:`Hide`;'>Show</button></div></div><div><label>Student ID</label><input name='student_id' placeholder='Enter student ID'></div><div><label>Batch</label><input name='batch' value='25' readonly></div><div><label>Semester</label><input name='semester' value='4th Semester' readonly></div><button type='submit'>Create Account</button><a class='button ghost' href='/login'>Back to Login</a></form></section></div></div>");
    page_end(&s); return s_take(&s);
}

static char *page_dashboard(void) {
    String s; s_init(&s); layout_start(&s, "Dashboard", "Dashboard", "Live-style update feed using C backend, SQLite database and HTML refresh. No JavaScript is used.", 1);
    int students = scalar_int("SELECT COUNT(*) FROM students");
    int teachers = scalar_int("SELECT COUNT(*) FROM teachers");
    int courses = scalar_int("SELECT COUNT(*) FROM courses");
    int notices = scalar_int("SELECT COUNT(*) FROM notices WHERE status='Active'");
    int reports = scalar_int("SELECT COUNT(*) FROM student_reports WHERE status='Open'");
    int labs = scalar_int("SELECT COUNT(*) FROM labs");
    double credit = scalar_double("SELECT COALESCE(SUM(credit),0) FROM courses");
    s_appendf(&s, "<div class='cards'><div class='card c1'><span>Total Students</span><b>%d</b><small>CSE 25 Batch</small></div><div class='card c2'><span>CSE Teachers</span><b>%d</b><small>Official faculty list</small></div><div class='card c3'><span>Courses</span><b>%d</b><small>%.1f total credits</small></div><div class='card c4'><span>Active Notices</span><b>%d</b><small>Department, teacher and CR</small></div><div class='card c5'><span>Open Reports</span><b>%d</b><small>Student feedback</small></div><div class='card c6'><span>Labs</span><b>%d</b><small>Routine based lab rooms</small></div></div>", students, teachers, courses, credit, notices, reports, labs);
    s_append(&s, "<div class='grid-main'><section class='panel'><h2>Latest Updates</h2>");
    const char *h1[] = {"Date","Title","From","Priority"};
    db_table(&s, "SELECT publish_date,title,notice_from,priority FROM notices ORDER BY id DESC LIMIT 6", h1, 4);
    s_append(&s, "</section><section class='panel'><h2>Recent Activity</h2>");
    const char *h2[] = {"Time","Actor","Action","Details"};
    db_table(&s, "SELECT created_at,actor,action,details FROM activity_logs ORDER BY id DESC LIMIT 8", h2, 4);
    s_append(&s, "</section></div><section class='panel'><h2>Mail Alert Log</h2>");
    const char *h3[] = {"Time","Recipient","Subject","Status"};
    db_table(&s, "SELECT created_at,recipient,subject,status FROM mail_logs ORDER BY id DESC LIMIT 8", h3, 4);
    s_append(&s, "</section>");
    layout_end(&s); return s_take(&s);
}

static char *page_department(void) {
    String s; s_init(&s); layout_start(&s, "Department", "Department", "Official CSE department summary, web photos, news feed and university facilities information.", 0);
    s_append(&s, "<section class='hero dept-hero'><div><h2>Computer Science & Engineering</h2><p>The CSE Department of Pundra University of Science & Technology focuses on research, CSE and ICT education, modern laboratories, seminar culture, programming practice and project-based learning for CSE 25 Batch, 4th Semester.</p></div><div class='hero-badge'>Pundra University<br>CSE Department<br>25 Batch</div></section>");
    s_append(&s, "<section class='panel colorful-panel'><h2>Department Photo Board</h2><p class='muted'>Images are loaded from Pundra University web resources. If internet is off, the cards still keep the department information.</p><div class='dept-image-grid'>");
    sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(g_db,"SELECT title,description,image_url,source_url FROM department_images ORDER BY id",-1,&st,NULL)==SQLITE_OK){
        while(sqlite3_step(st)==SQLITE_ROW){
            char *title=html_escape(safe_text(st,0)); char *desc=html_escape(safe_text(st,1)); char *img=html_escape(safe_text(st,2)); char *src=html_escape(safe_text(st,3));
            s_appendf(&s,"<article class='dept-photo'><img src='%s' alt='%s'><div><h3>%s</h3><p>%s</p><a href='%s' target='_blank'>Source page</a></div></article>",img,title,title,desc,src);
            free(title); free(desc); free(img); free(src);
        }
    }
    sqlite3_finalize(st);
    s_append(&s,"</div></section>");
    s_append(&s, "<div class='grid3'><div class='panel vision-card'><h2>Vision</h2><p>Student learning is the top priority. Students develop technology skills, individual talents and critical thinking toward excellence in engineering and technology education and research.</p></div><div class='panel mission-card'><h2>Mission</h2><p>Provide quality CSE education, produce competent engineers, contribute through research and innovation, strengthen industry interaction and build leadership with ethics.</p></div><div class='panel facility-card'><h2>Facilities</h2><p>Modern computer laboratories, Wi-Fi, multimedia classrooms, library collections, academic contests, seminars and co-curricular activities support student learning.</p></div></div>");
    s_append(&s, "<section class='panel'><h2>Department News Feed</h2>");
    const char *h[] = {"Date","Title","From","Message"};
    db_table(&s, "SELECT publish_date,title,notice_from,message FROM notices ORDER BY id DESC LIMIT 8", h, 4);
    s_append(&s, "</section>");
    layout_end(&s); return s_take(&s);
}

static char *page_teachers(void) {
    String s; s_init(&s); layout_start(&s, "Teachers", "Teachers", "CSE teacher cards with official department faculty information and photos loaded from Pundra University site.", 0);
    sqlite3_stmt *st = NULL;
    s_append(&s, "<div class='teacher-grid'>");
    if (sqlite3_prepare_v2(g_db, "SELECT full_name,short_form,designation,mobile,email,photo_url FROM teachers ORDER BY CASE WHEN designation LIKE '%Head%' THEN 0 ELSE 1 END, id", -1, &st, NULL)==SQLITE_OK) {
        while (sqlite3_step(st)==SQLITE_ROW) {
            char *name=html_escape(safe_text(st,0)); char *sf=html_escape(safe_text(st,1)); char *des=html_escape(safe_text(st,2)); char *mob=html_escape(safe_text(st,3)); char *email=html_escape(safe_text(st,4)); char *photo=html_escape(safe_text(st,5));
            s_appendf(&s,"<article class='teacher-card'><div class='photo-wrap'><img src='%s' alt='%s'></div><div><h2>%s</h2><p class='role'>%s</p><div class='pill'>%s</div><p class='small'>Mobile: %s</p><p class='small'>Email: %s</p></div></article>",photo,name,name,des,sf,mob,email);
            free(name); free(sf); free(des); free(mob); free(email); free(photo);
        }
    }
    sqlite3_finalize(st);
    s_append(&s, "</div>");
    layout_end(&s); return s_take(&s);
}

static char *page_students(void) {
    String s; s_init(&s); layout_start(&s, "Students", "Students", "CSE 25 Batch 4th Semester student list provided by you. Mark values were not added.", 0);
    s_append(&s, "<section class='panel'><h2>Add Student</h2><form method='post' action='/student/add' class='form inline-form'><input name='roll_no' placeholder='Roll'><input name='student_id' placeholder='Student ID' required><input name='full_name' placeholder='Full Name' required><button>Add Student</button></form></section>");
    const char *h[] = {"Roll","Student ID","Name","Department","Batch","Semester","Phone","Status"};
    db_table(&s, "SELECT roll_no,student_id,full_name,department,batch,semester,phone,status FROM students ORDER BY CAST(roll_no AS INTEGER)", h, 8);
    layout_end(&s); return s_take(&s);
}

static char *page_courses(void) {
    String s; s_init(&s); layout_start(&s, "Courses", "Courses", "Colorful 2nd Year 2nd Semester course list with credit and teacher mapping from your routine.", 0);
    double total_credit = scalar_double("SELECT COALESCE(SUM(credit),0) FROM courses");
    int total_courses = scalar_int("SELECT COUNT(*) FROM courses");
    s_appendf(&s,"<section class='course-banner'><div><h2>CSE 25 Batch Course Plan</h2><p>2nd Year 2nd Semester / 4th Semester. Total courses: %d. Total credit: %.2f.</p></div><a class='button light-btn' href='/results'>Open CGPA Calculator</a></section>",total_courses,total_credit);
    s_append(&s,"<div class='course-grid'>");
    sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(g_db,"SELECT course_code,course_title,credit,course_type,teacher_name,teacher_short,room FROM courses ORDER BY id",-1,&st,NULL)==SQLITE_OK){
        while(sqlite3_step(st)==SQLITE_ROW){
            char *code=html_escape(safe_text(st,0)); char *title=html_escape(safe_text(st,1)); char *type=html_escape(safe_text(st,3)); char *teacher=html_escape(safe_text(st,4)); char *shortf=html_escape(safe_text(st,5)); char *room=html_escape(safe_text(st,6));
            s_appendf(&s,"<article class='course-card'><div class='course-code'>%s</div><h2>%s</h2><div class='course-meta'><span>%.2f Credit</span><span>%s</span></div><p>Teacher: <b>%s</b> (%s)</p><p>Room: %s</p></article>",code,title,sqlite3_column_double(st,2),type,teacher,shortf,room);
            free(code); free(title); free(type); free(teacher); free(shortf); free(room);
        }
    }
    sqlite3_finalize(st);
    s_append(&s,"</div><section class='panel'><h2>Course Table</h2>");
    const char *h[] = {"Code","Course Title","Credit","Type","Teacher","Short","Room"};
    db_table(&s, "SELECT course_code,course_title,printf('%.2f',credit),course_type,teacher_name,teacher_short,room FROM courses ORDER BY id", h, 7);
    s_append(&s,"</section>");
    layout_end(&s); return s_take(&s);
}

static char *page_routine(void) {
    String s; s_init(&s); layout_start(&s, "Routine", "Routine", "Default CSE 25 Batch routine image and editable routine slots.", 0);
    char img[512]="/images/cse25_routine.svg", note[512]="Default routine recreated from the uploaded routine image.";
    sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(g_db,"SELECT image_url,note FROM routine_assets WHERE title='CSE 25 Batch Default Routine' LIMIT 1",-1,&st,NULL)==SQLITE_OK && sqlite3_step(st)==SQLITE_ROW){
        strncpy(img,safe_text(st,0),sizeof(img)-1); strncpy(note,safe_text(st,1),sizeof(note)-1);
    }
    sqlite3_finalize(st);
    char *eimg=html_escape(img); char *enote=html_escape(note);
    s_appendf(&s,"<section class='routine-hero'><div><h2>CSE 25 Batch Class Routine</h2><p>%s</p></div><div class='routine-actions'><a class='button light-btn' href='/attendance'>Attendance</a><a class='button light-btn' href='/results'>Result Publish</a></div></section><section class='panel routine-panel'><h2>Default Routine Image</h2><div class='routine-img-wrap'><img src='%s' alt='CSE 25 Batch routine'></div></section>",enote,eimg);
    s_appendf(&s,"<section class='panel'><h2>Change Routine Image</h2><p class='muted'>To use your own picture, copy it inside public/images and write the path like /images/my_routine.png, or use a web image URL.</p><form method='post' action='/routine/image/save' class='form grid2'><input name='image_url' value='%s' placeholder='/images/cse25_routine.svg'><input name='note' value='%s' placeholder='Routine note'><button>Save Routine Image</button></form></section>",eimg,enote);
    free(eimg); free(enote);
    s_append(&s,"<section class='panel'><h2>Add / Update Routine Slot</h2><form method='post' action='/routine/slot/add' class='form grid4'><select name='day_name'><option>Saturday</option><option>Sunday</option><option>Monday</option><option>Tuesday</option><option>Wednesday</option><option>Thursday</option></select><input name='time_slot' placeholder='09:30 AM - 10:45 AM'><select name='course_code'>");
    if(sqlite3_prepare_v2(g_db,"SELECT course_code,course_title FROM courses ORDER BY id",-1,&st,NULL)==SQLITE_OK) while(sqlite3_step(st)==SQLITE_ROW){ char *c=html_escape(safe_text(st,0)); char *t=html_escape(safe_text(st,1)); s_appendf(&s,"<option value='%s'>%s - %s</option>",c,c,t); free(c); free(t);} sqlite3_finalize(st);
    s_append(&s,"</select><input name='teacher_short' placeholder='Teacher short'><input name='room_no' placeholder='Room'><input name='note' placeholder='Note'><button>Save Slot</button></form></section>");
    s_append(&s,"<section class='panel'><h2>Routine Slots</h2>"); const char *h[]={"Day","Time","Course","Teacher","Room","Note"}; db_table(&s,"SELECT day_name,time_slot,course_code,teacher_short,room_no,note FROM routine_slots ORDER BY CASE day_name WHEN 'Saturday' THEN 1 WHEN 'Sunday' THEN 2 WHEN 'Monday' THEN 3 WHEN 'Tuesday' THEN 4 WHEN 'Wednesday' THEN 5 WHEN 'Thursday' THEN 6 ELSE 7 END, time_slot",h,6); s_append(&s,"</section>");
    layout_end(&s); return s_take(&s);
}

static char *page_results(const char *msg) {
    String s; s_init(&s); layout_start(&s, "Result Publish", "Result Publish", "Credit-wise CGPA calculator and published result sheet for CSE 25 Batch, 4th Semester.", 0);
    if(msg && *msg) s_appendf(&s,"<div class='alert ok'>%s</div>",msg);
    double total_credit=scalar_double("SELECT COALESCE(SUM(credit),0) FROM courses");
    double earned_credit=scalar_double("SELECT COALESCE(SUM(credit),0) FROM result_records WHERE grade_point>0");
    double weighted=scalar_double("SELECT COALESCE(SUM(credit*grade_point),0) FROM result_records");
    double cgpa = earned_credit>0 ? weighted/earned_credit : 0.0;
    s_appendf(&s,"<div class='cards result-cards'><div class='card c1'><span>Total Course Credit</span><b>%.1f</b><small>Semester credit</small></div><div class='card c3'><span>Published Credit</span><b>%.1f</b><small>Passed credit only</small></div><div class='card c2'><span>Current CGPA</span><b>%.2f</b><small>Credit weighted</small></div></div>",total_credit,earned_credit,cgpa);
    s_append(&s,"<section class='panel result-panel'><h2>Publish / Calculate Result</h2><p class='muted'>Select student, select course, enter marks. Backend calculates grade point, letter grade and CGPA from credit.</p><form method='post' action='/result/publish' class='form grid4'><select name='student_id'>");
    sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(g_db,"SELECT student_id,full_name FROM students ORDER BY CAST(roll_no AS INTEGER)",-1,&st,NULL)==SQLITE_OK) while(sqlite3_step(st)==SQLITE_ROW){ char *id=html_escape(safe_text(st,0)); char *n=html_escape(safe_text(st,1)); s_appendf(&s,"<option value='%s'>%s - %s</option>",id,id,n); free(id); free(n);} sqlite3_finalize(st);
    s_append(&s,"</select><select name='course_code'>");
    if(sqlite3_prepare_v2(g_db,"SELECT course_code,course_title,credit FROM courses ORDER BY id",-1,&st,NULL)==SQLITE_OK) while(sqlite3_step(st)==SQLITE_ROW){ char *c=html_escape(safe_text(st,0)); char *t=html_escape(safe_text(st,1)); s_appendf(&s,"<option value='%s'>%s - %s (%.2f credit)</option>",c,c,t,sqlite3_column_double(st,2)); free(c); free(t);} sqlite3_finalize(st);
    s_append(&s,"</select><input type='number' step='0.01' min='0' max='100' name='marks' placeholder='Marks out of 100'><button>Publish Result</button></form></section>");
    s_append(&s,"<section class='panel'><h2>Grade Scale</h2><div class='grade-scale'><span>A+ 80-100 = 4.00</span><span>A 75-79 = 3.75</span><span>A- 70-74 = 3.50</span><span>B+ 65-69 = 3.25</span><span>B 60-64 = 3.00</span><span>C/D/F below</span></div></section>");
    const char *h[]={"Time","Student ID","Name","Course","Credit","Marks","Grade","Point"};
    db_table(&s,"SELECT publish_date,student_id,student_name,course_code,printf('%.2f',credit),printf('%.2f',marks),letter_grade,printf('%.2f',grade_point) FROM result_records ORDER BY id DESC",h,8);
    layout_end(&s); return s_take(&s);
}

static char *page_attendance(void) {
    String s; s_init(&s); layout_start(&s, "Attendance", "Attendance", "Course-wise daily attendance update. Select date, course and student status.", 0);
    char today[32]; today_string(today, sizeof(today));
    s_appendf(&s, "<section class='panel'><h2>Update Attendance</h2><form method='post' action='/attendance/add' class='form grid4'><div><label>Date</label><input type='date' name='date' value='%s' required></div><div><label>Course</label><select name='course_code'>", today);
    sqlite3_stmt *st=NULL;
    if (sqlite3_prepare_v2(g_db,"SELECT course_code,course_title FROM courses ORDER BY id",-1,&st,NULL)==SQLITE_OK) while(sqlite3_step(st)==SQLITE_ROW){ char *c=html_escape(safe_text(st,0)); char *t=html_escape(safe_text(st,1)); s_appendf(&s,"<option value='%s'>%s - %s</option>",c,c,t); free(c); free(t);} sqlite3_finalize(st);
    s_append(&s, "</select></div><div><label>Student</label><select name='student_id'>");
    if (sqlite3_prepare_v2(g_db,"SELECT student_id,full_name FROM students ORDER BY CAST(roll_no AS INTEGER)",-1,&st,NULL)==SQLITE_OK) while(sqlite3_step(st)==SQLITE_ROW){ char *id=html_escape(safe_text(st,0)); char *n=html_escape(safe_text(st,1)); s_appendf(&s,"<option value='%s'>%s - %s</option>",id,id,n); free(id); free(n);} sqlite3_finalize(st);
    s_append(&s, "</select></div><div><label>Status</label><select name='status'><option>Present</option><option>Absent</option><option>Late</option></select></div><button>Save Attendance</button></form></section>");
    const char *h[]={"Date","Course","Student ID","Status","By","Time"};
    db_table(&s,"SELECT date,course_code,student_id,status,updated_by,created_at FROM attendance ORDER BY id DESC LIMIT 80",h,6);
    layout_end(&s); return s_take(&s);
}

static char *page_ct_exams(void) {
    String s; s_init(&s); layout_start(&s, "CT & Exams", "CT & Exams", "Course-wise CT, lab final, assignment, lab report and exam plan.", 0);
    s_append(&s,"<section class='panel'><h2>Add CT or Exam</h2><form method='post' action='/exam/add' class='form grid4'><input name='course_code' placeholder='Course Code'><input name='title' placeholder='Title'><select name='exam_type'><option>Class Test</option><option>Lab Final</option><option>Assignment</option><option>Lab Report</option><option>Final</option></select><input type='date' name='exam_date'><input name='full_mark' placeholder='Full Mark'><input name='note' placeholder='Note'><button>Add</button></form></section>");
    const char *h[]={"Course","Title","Type","Date","Full Mark","Note","Status"};
    db_table(&s,"SELECT course_code,title,exam_type,exam_date,printf('%.0f',full_mark),note,status FROM ct_exams ORDER BY exam_date,id",h,7);
    layout_end(&s); return s_take(&s);
}

static char *page_notices(void) {
    String s; s_init(&s); layout_start(&s, "Notices", "Notices", "Department, course teacher and CR notice board with mail alert logging.", 0);
    char today[32]; today_string(today,sizeof(today));
    s_appendf(&s,"<section class='panel'><h2>Publish Notice</h2><form method='post' action='/notice/add' class='form grid2'><input name='title' placeholder='Notice title' required><input name='course_code' placeholder='Course Code or ALL'><select name='notice_from'><option>Department</option><option>Course Teacher</option><option>CR</option><option>Admin</option></select><select name='priority'><option>High</option><option>Medium</option><option>Low</option></select><input type='date' name='publish_date' value='%s'><textarea name='message' placeholder='Notice details' required></textarea><button>Publish Notice</button></form></section>",today);
    const char *h[]={"Date","Title","Course","From","Priority","Message"};
    db_table(&s,"SELECT publish_date,title,course_code,notice_from,priority,message FROM notices ORDER BY id DESC",h,6);
    layout_end(&s); return s_take(&s);
}

static char *page_groups(void) {
    String s; s_init(&s); layout_start(&s, "Groups", "Groups", "Research, website project, robotics and programming groups with registration request.", 0);
    s_append(&s,"<section class='panel'><h2>Group Registration</h2><form method='post' action='/group/join' class='form grid2'><select name='group_id'>");
    sqlite3_stmt *st=NULL; if(sqlite3_prepare_v2(g_db,"SELECT id,group_name FROM groups_table ORDER BY id",-1,&st,NULL)==SQLITE_OK) while(sqlite3_step(st)==SQLITE_ROW){ s_appendf(&s,"<option value='%d'>%s</option>",sqlite3_column_int(st,0),safe_text(st,1)); } sqlite3_finalize(st);
    s_append(&s,"</select><input name='student_id' placeholder='Student ID'><input name='student_name' placeholder='Student Name'><textarea name='reason' placeholder='Why do you want to join?'></textarea><button>Send Request</button></form></section>");
    const char *h1[]={"Group","Focus","Leader","Meeting","Status","Details"}; db_table(&s,"SELECT group_name,focus_area,leader,meeting_time,status,description FROM groups_table ORDER BY id",h1,6);
    s_append(&s,"<section class='panel'><h2>Pending/Approved Group Requests</h2>"); const char *h2[]={"Time","Group","Student ID","Name","Status"}; db_table(&s,"SELECT gm.created_at,g.group_name,gm.student_id,gm.student_name,gm.status FROM group_members gm LEFT JOIN groups_table g ON g.id=gm.group_id ORDER BY gm.id DESC LIMIT 40",h2,5); s_append(&s,"</section>");
    layout_end(&s); return s_take(&s);
}

static char *page_library(void) {
    String s; s_init(&s); layout_start(&s, "Library", "Library", "CSE related books, copy count, issue/return registration and waiting style record.", 0);
    s_append(&s,"<section class='panel'><h2>Issue Book</h2><form method='post' action='/library/issue' class='form grid4'><select name='book_id'>");
    sqlite3_stmt *st=NULL; if(sqlite3_prepare_v2(g_db,"SELECT id,title,available_copy FROM library_books ORDER BY title",-1,&st,NULL)==SQLITE_OK) while(sqlite3_step(st)==SQLITE_ROW){ s_appendf(&s,"<option value='%d'>%s (%d available)</option>",sqlite3_column_int(st,0),safe_text(st,1),sqlite3_column_int(st,2)); } sqlite3_finalize(st);
    char today[32]; today_string(today,sizeof(today));
    s_appendf(&s,"</select><input name='student_id' placeholder='Student ID'><input name='student_name' placeholder='Student Name'><input type='date' name='issue_date' value='%s'><input type='date' name='due_date'><button>Issue / Request</button></form></section>",today);
    const char *h[]={"Book","Author","Category","Total","Available","Shelf"}; db_table(&s,"SELECT title,author,category,total_copy,available_copy,shelf FROM library_books ORDER BY title",h,6);
    s_append(&s,"<section class='panel'><h2>Issue Records</h2>"); const char *h2[]={"Book","Student ID","Student Name","Issue","Due","Return","Status"}; db_table(&s,"SELECT b.title,i.student_id,i.student_name,i.issue_date,i.due_date,COALESCE(i.return_date,''),i.status FROM library_issues i LEFT JOIN library_books b ON b.id=i.book_id ORDER BY i.id DESC LIMIT 60",h2,7); s_append(&s,"</section>");
    layout_end(&s); return s_take(&s);
}

static char *page_canteen(void) {
    String s; s_init(&s); layout_start(&s, "Canteen", "Canteen", "Local food menu, price update view and bill calculation through C backend form submit.", 0);
    s_append(&s,"<section class='panel'><h2>Create Bill</h2><form method='post' action='/canteen/order' class='form grid4'><input name='customer_name' placeholder='Customer name'><select name='item_id'>");
    sqlite3_stmt *st=NULL; if(sqlite3_prepare_v2(g_db,"SELECT id,item_name,price FROM canteen_items ORDER BY item_name",-1,&st,NULL)==SQLITE_OK) while(sqlite3_step(st)==SQLITE_ROW){ s_appendf(&s,"<option value='%d'>%s - %.2f Tk</option>",sqlite3_column_int(st,0),safe_text(st,1),sqlite3_column_double(st,2)); } sqlite3_finalize(st);
    s_append(&s,"</select><input type='number' name='quantity' value='1' min='1'><button>Calculate & Save</button></form></section>");
    const char *h[]={"Item","Price","Category","Status"}; db_table(&s,"SELECT item_name,printf('%.2f Tk',price),category,status FROM canteen_items ORDER BY category,item_name",h,4);
    s_append(&s,"<section class='panel'><h2>Recent Bills</h2>"); const char *h2[]={"Time","Customer","Item","Qty","Total"}; db_table(&s,"SELECT order_date,customer_name,item_name,quantity,printf('%.2f Tk',total) FROM canteen_orders ORDER BY id DESC LIMIT 30",h2,5); s_append(&s,"</section>");
    layout_end(&s); return s_take(&s);
}

static char *page_blood(void) {
    String s; s_init(&s); layout_start(&s, "Blood Donation", "Blood Donation", "CSE 25 Batch, 4th Semester donor profiles. One Student ID can be registered only once; new submit updates existing donor details.", 0);
    s_append(&s,"<section class='panel'><h2>Register Donor</h2><form method='post' action='/blood/add' class='form grid4'><input name='student_id' placeholder='Student ID'><input name='full_name' placeholder='Full Name'><select name='blood_group'><option>A+</option><option>B+</option><option>O+</option><option>AB+</option><option>A-</option><option>B-</option><option>O-</option><option>AB-</option></select><input name='phone' placeholder='Phone'><input name='last_donation' placeholder='Last donation, e.g. 5 months ago'><select name='availability'><option>Available</option><option>Need confirmation</option><option>Unavailable</option></select><button>Register</button></form></section>");
    const char *h[]={"Student ID","Name","Blood","Phone","Last Donation","Availability","Note"}; db_table(&s,"SELECT student_id,full_name,blood_group,phone,last_donation,availability,note FROM blood_donors ORDER BY full_name",h,7);
    layout_end(&s); return s_take(&s);
}

static char *page_labs(void) {
    String s; s_init(&s); layout_start(&s, "Lab Management", "Lab Management", "Computer labs, microprocessor lab and routine room information based on Pundra facilities and routine rooms.", 0);
    const char *h[]={"Lab","Room","PC","Available","Assistant","Status","Details"}; db_table(&s,"SELECT lab_name,room_no,pc_count,available_pc,lab_assistant,status,details FROM labs ORDER BY id",h,7);
    layout_end(&s); return s_take(&s);
}

static char *page_reports(void) {
    String s; s_init(&s); layout_start(&s, "Student Reports", "Student Reports", "Students can submit class, course, lab, library, canteen or other reports.", 0);
    s_append(&s,"<section class='panel'><h2>Submit Report</h2><form method='post' action='/report/add' class='form grid2'><input name='student_id' placeholder='Student ID'><input name='student_name' placeholder='Student Name'><select name='report_type'><option>Class problem</option><option>Lab problem</option><option>Course problem</option><option>Library problem</option><option>Canteen problem</option><option>Notice problem</option><option>Other</option></select><textarea name='details' placeholder='Write details'></textarea><button>Submit Report</button></form></section>");
    const char *h[]={"Time","Student ID","Name","Type","Details","Status"}; db_table(&s,"SELECT created_at,student_id,student_name,report_type,details,status FROM student_reports ORDER BY id DESC",h,6);
    layout_end(&s); return s_take(&s);
}

static char *page_activity(void) {
    String s; s_init(&s); layout_start(&s, "Activity", "Activity", "Login, settings, attendance, notice, library and form update history.", 1);
    const char *h[]={"Time","Actor","Action","Details"}; db_table(&s,"SELECT created_at,actor,action,details FROM activity_logs ORDER BY id DESC LIMIT 200",h,4);
    layout_end(&s); return s_take(&s);
}

static char *page_settings(void) {
    String s; s_init(&s); layout_start(&s, "Settings", "Settings", "Portal settings and SMTP/API-ready mail alert configuration. Default mode saves mail logs only.", 0);
    s_append(&s,"<section class='panel'><h2>Mail Alert Settings</h2><p class='muted'>This project logs mail alerts by default. To send real email, add SMTP details and extend the send_mail function with your provider settings.</p><form method='post' action='/settings/save' class='form grid2'><input name='smtp_host' placeholder='SMTP Host'><input name='smtp_port' placeholder='SMTP Port' value='587'><input name='smtp_user' placeholder='SMTP User'><input name='smtp_password' type='password' placeholder='SMTP Password'><button>Save Settings</button></form></section>");
    const char *h[]={"Key","Value"}; db_table(&s,"SELECT key,CASE WHEN key LIKE '%password%' THEN 'Hidden' ELSE value END FROM settings ORDER BY key",h,2);
    layout_end(&s); return s_take(&s);
}

static char *page_password(const char *msg) {
    String s; s_init(&s); layout_start(&s, "Password", "Password", "Change admin/user password. Login page has a Show/Hide password option.", 0);
    if(msg && *msg) s_appendf(&s,"<div class='alert ok'>%s</div>",msg);
    s_append(&s,"<section class='panel max700'><h2>Update Password</h2><form method='post' action='/password/change' class='form'><label>Current Password</label><input type='password' name='old_password' required><label>New Password</label><input type='password' name='new_password' required><label>Confirm New Password</label><input type='password' name='confirm_password' required><button>Update Password</button></form></section>");
    layout_end(&s); return s_take(&s);
}

static char *page_not_found(void) {
    String s; s_init(&s); page_head(&s,"Not Found",0); s_append(&s,"<div class='auth-bg'><div class='auth-card'><h1>Page not found</h1><a class='button' href='/dashboard'>Back to Dashboard</a></div></div>"); page_end(&s); return s_take(&s);
}

/* ------------------------- post handlers ------------------------- */
static int valid_login(const char *email, const char *password) {
    sqlite3_stmt *st=NULL; char hash[32]; password_hash_hex(password, hash); int ok=0;
    if(sqlite3_prepare_v2(g_db,"SELECT id FROM users WHERE email=? AND password_hash=? AND status='Active'",-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,email,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,hash,-1,SQLITE_TRANSIENT); if(sqlite3_step(st)==SQLITE_ROW) ok=1; }
    sqlite3_finalize(st); return ok;
}

static void post_login(socket_t client, const char *body) {
    char *email=form_value(body,"email"), *password=form_value(body,"password");
    if(valid_login(email,password)) { log_activity(email,"Login","User logged in successfully."); redirect_to(client,"/dashboard","Set-Cookie: pucse_session=active; Path=/; HttpOnly\r\n"); }
    else { char *p=page_login("Invalid email or password."); send_raw(client,"200 OK","text/html",p,NULL); free(p); }
    free(email); free(password);
}

static void post_register(socket_t client, const char *body) {
    char *name=form_value(body,"full_name"),*email=form_value(body,"email"),*password=form_value(body,"password"),*sid=form_value(body,"student_id"),*batch=form_value(body,"batch"),*sem=form_value(body,"semester");
    char hash[32]; password_hash_hex(password,hash); sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(g_db,"INSERT OR IGNORE INTO users(full_name,email,password_hash,role,department,batch,semester,status) VALUES(?,?,?,?,?,?,?,?)",-1,&st,NULL)==SQLITE_OK){
        sqlite3_bind_text(st,1,name,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,email,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,hash,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,"Student",-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,5,"Computer Science & Engineering",-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,6,batch,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,7,sem,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,8,"Active",-1,SQLITE_TRANSIENT); sqlite3_step(st);
    } sqlite3_finalize(st);
    log_activity(email,"Registration","New student registration activated instantly."); add_mail_log(email,"Registration successful","Your CSE 25 Batch 4th Semester account is active.","logged");
    char *p=page_register("Registration successful. You can login now."); send_raw(client,"200 OK","text/html",p,NULL); free(p);
    free(name); free(email); free(password); free(sid); free(batch); free(sem);
}

static void post_student_add(socket_t client, const char *body) {
    char *roll=form_value(body,"roll_no"),*sid=form_value(body,"student_id"),*name=form_value(body,"full_name"); sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(g_db,"INSERT OR IGNORE INTO students(roll_no,student_id,full_name,department,batch,semester,phone,status) VALUES(?,?,?,?,?,?,?,?)",-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,roll,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,sid,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,name,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,"Computer Science & Engineering",-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,5,"25",-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,6,"4th Semester",-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,7,"Update required",-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,8,"Active",-1,SQLITE_TRANSIENT); sqlite3_step(st); } sqlite3_finalize(st);
    log_activity("admin","Student added",name); redirect_to(client,"/students",NULL); free(roll); free(sid); free(name);
}

static void post_attendance_add(socket_t client, const char *body) {
    char *date=form_value(body,"date"),*course=form_value(body,"course_code"),*sid=form_value(body,"student_id"),*status=form_value(body,"status"); sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(g_db,"INSERT OR REPLACE INTO attendance(date,course_code,student_id,status,note,updated_by,created_at) VALUES(?,?,?,?,?,?,datetime('now','localtime'))",-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,date,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,course,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,sid,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,status,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,5,"Updated from web portal",-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,6,"admin",-1,SQLITE_TRANSIENT); sqlite3_step(st);} sqlite3_finalize(st);
    char details[512]; snprintf(details,sizeof(details),"%s %s %s",date,course,status); log_activity("admin","Attendance updated",details); add_mail_log("batch25@local","Attendance update",details,"logged"); redirect_to(client,"/attendance",NULL); free(date); free(course); free(sid); free(status);
}

static void post_exam_add(socket_t client, const char *body){ char *course=form_value(body,"course_code"),*title=form_value(body,"title"),*type=form_value(body,"exam_type"),*date=form_value(body,"exam_date"),*mark=form_value(body,"full_mark"),*note=form_value(body,"note"); sqlite3_stmt *st=NULL; if(sqlite3_prepare_v2(g_db,"INSERT INTO ct_exams(course_code,title,exam_type,exam_date,full_mark,note,status) VALUES(?,?,?,?,?,?,?)",-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,course,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,title,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,type,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,date,-1,SQLITE_TRANSIENT); sqlite3_bind_double(st,5,atof(mark)); sqlite3_bind_text(st,6,note,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,7,"Scheduled",-1,SQLITE_TRANSIENT); sqlite3_step(st);} sqlite3_finalize(st); log_activity("admin","Exam added",title); add_mail_log("batch25@local","Exam update",title,"logged"); redirect_to(client,"/ct-exams",NULL); free(course); free(title); free(type); free(date); free(mark); free(note); }

static void post_routine_image_save(socket_t client, const char *body){
    char *img=form_value(body,"image_url"), *note=form_value(body,"note");
    sqlite3_stmt *st=NULL;
    const char *sql="INSERT INTO routine_assets(title,image_url,note,updated_at) VALUES('CSE 25 Batch Default Routine',?,?,datetime('now','localtime')) ON CONFLICT(title) DO UPDATE SET image_url=excluded.image_url,note=excluded.note,updated_at=datetime('now','localtime')";
    if(sqlite3_prepare_v2(g_db,sql,-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,img,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,note,-1,SQLITE_TRANSIENT); sqlite3_step(st); }
    sqlite3_finalize(st); log_activity("admin","Routine image changed",img); redirect_to(client,"/routine",NULL); free(img); free(note);
}

static void post_routine_slot_add(socket_t client, const char *body){
    char *day=form_value(body,"day_name"), *time=form_value(body,"time_slot"), *course=form_value(body,"course_code"), *teacher=form_value(body,"teacher_short"), *room=form_value(body,"room_no"), *note=form_value(body,"note");
    sqlite3_stmt *st=NULL;
    const char *sql="INSERT INTO routine_slots(day_name,time_slot,course_code,teacher_short,room_no,note) VALUES(?,?,?,?,?,?) ON CONFLICT(day_name,time_slot,course_code) DO UPDATE SET teacher_short=excluded.teacher_short,room_no=excluded.room_no,note=excluded.note";
    if(sqlite3_prepare_v2(g_db,sql,-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,day,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,time,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,course,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,teacher,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,5,room,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,6,note,-1,SQLITE_TRANSIENT); sqlite3_step(st); }
    sqlite3_finalize(st); log_activity("admin","Routine slot saved",course); add_mail_log("batch25@local","Routine update",course,"logged"); redirect_to(client,"/routine",NULL); free(day); free(time); free(course); free(teacher); free(room); free(note);
}

static void post_result_publish(socket_t client, const char *body){
    char *sid=form_value(body,"student_id"), *course=form_value(body,"course_code"), *marks_s=form_value(body,"marks");
    double marks=atof(marks_s); if(marks<0) marks=0; if(marks>100) marks=100;
    char student_name[256]="", course_title[256]=""; double credit=0;
    sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(g_db,"SELECT full_name FROM students WHERE student_id=?",-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,sid,-1,SQLITE_TRANSIENT); if(sqlite3_step(st)==SQLITE_ROW) strncpy(student_name,safe_text(st,0),sizeof(student_name)-1); }
    sqlite3_finalize(st); st=NULL;
    if(sqlite3_prepare_v2(g_db,"SELECT course_title,credit FROM courses WHERE course_code=?",-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,course,-1,SQLITE_TRANSIENT); if(sqlite3_step(st)==SQLITE_ROW){ strncpy(course_title,safe_text(st,0),sizeof(course_title)-1); credit=sqlite3_column_double(st,1); }}
    sqlite3_finalize(st); st=NULL;
    double gp=grade_point_from_mark(marks); const char *lg=letter_from_mark(marks);
    const char *sql="INSERT INTO result_records(student_id,student_name,course_code,course_title,credit,marks,letter_grade,grade_point,published_by,publish_date) VALUES(?,?,?,?,?,?,?,?,?,datetime('now','localtime')) ON CONFLICT(student_id,course_code) DO UPDATE SET student_name=excluded.student_name,course_title=excluded.course_title,credit=excluded.credit,marks=excluded.marks,letter_grade=excluded.letter_grade,grade_point=excluded.grade_point,published_by=excluded.published_by,publish_date=datetime('now','localtime')";
    if(sqlite3_prepare_v2(g_db,sql,-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,sid,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,student_name,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,course,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,course_title,-1,SQLITE_TRANSIENT); sqlite3_bind_double(st,5,credit); sqlite3_bind_double(st,6,marks); sqlite3_bind_text(st,7,lg,-1,SQLITE_TRANSIENT); sqlite3_bind_double(st,8,gp); sqlite3_bind_text(st,9,"admin",-1,SQLITE_TRANSIENT); sqlite3_step(st); }
    sqlite3_finalize(st);
    char msg[512]; snprintf(msg,sizeof(msg),"Result published: %s - %s, marks %.2f, grade %s, point %.2f",sid,course,marks,lg,gp);
    log_activity("admin","Result published",msg); add_mail_log("batch25@local","Result published",msg,"logged");
    char *page=page_results("Result published and CGPA recalculated."); send_raw(client,"200 OK","text/html",page,NULL); free(page);
    free(sid); free(course); free(marks_s);
}

static void post_notice_add(socket_t client, const char *body){ char *title=form_value(body,"title"),*course=form_value(body,"course_code"),*from=form_value(body,"notice_from"),*priority=form_value(body,"priority"),*date=form_value(body,"publish_date"),*msg=form_value(body,"message"); sqlite3_stmt *st=NULL; if(sqlite3_prepare_v2(g_db,"INSERT INTO notices(title,course_code,notice_from,message,priority,publish_date,status) VALUES(?,?,?,?,?,?,?)",-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,title,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,course,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,from,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,msg,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,5,priority,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,6,date,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,7,"Active",-1,SQLITE_TRANSIENT); sqlite3_step(st);} sqlite3_finalize(st); log_activity("admin","Notice published",title); add_mail_log("batch25@local",title,msg,"logged"); redirect_to(client,"/notices",NULL); free(title); free(course); free(from); free(priority); free(date); free(msg); }

static void post_group_join(socket_t client, const char *body){ char *gid=form_value(body,"group_id"),*sid=form_value(body,"student_id"),*name=form_value(body,"student_name"),*reason=form_value(body,"reason"); sqlite3_stmt *st=NULL; if(sqlite3_prepare_v2(g_db,"INSERT INTO group_members(group_id,student_id,student_name,reason,status) VALUES(?,?,?,?,?)",-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_int(st,1,atoi(gid)); sqlite3_bind_text(st,2,sid,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,name,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,reason,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,5,"Pending",-1,SQLITE_TRANSIENT); sqlite3_step(st);} sqlite3_finalize(st); log_activity(name,"Group join request",reason); redirect_to(client,"/groups",NULL); free(gid); free(sid); free(name); free(reason); }

static void post_library_issue(socket_t client, const char *body){ char *bid=form_value(body,"book_id"),*sid=form_value(body,"student_id"),*name=form_value(body,"student_name"),*issue=form_value(body,"issue_date"),*due=form_value(body,"due_date"); sqlite3_stmt *st=NULL; if(sqlite3_prepare_v2(g_db,"INSERT INTO library_issues(book_id,student_id,student_name,issue_date,due_date,status) VALUES(?,?,?,?,?,?)",-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_int(st,1,atoi(bid)); sqlite3_bind_text(st,2,sid,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,name,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,issue,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,5,due,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,6,"Issued",-1,SQLITE_TRANSIENT); sqlite3_step(st);} sqlite3_finalize(st); sqlite3_stmt *up=NULL; if(sqlite3_prepare_v2(g_db,"UPDATE library_books SET available_copy=CASE WHEN available_copy>0 THEN available_copy-1 ELSE 0 END WHERE id=?",-1,&up,NULL)==SQLITE_OK){ sqlite3_bind_int(up,1,atoi(bid)); sqlite3_step(up);} sqlite3_finalize(up); log_activity("librarian","Book issued/requested",name); add_mail_log("library@local","Book issue update",name,"logged"); redirect_to(client,"/library",NULL); free(bid); free(sid); free(name); free(issue); free(due); }

static void post_canteen_order(socket_t client, const char *body){ char *cid=form_value(body,"customer_name"),*iid=form_value(body,"item_id"),*qtys=form_value(body,"quantity"); int qty=atoi(qtys); if(qty<=0) qty=1; sqlite3_stmt *st=NULL; char item[256]=""; double price=0; if(sqlite3_prepare_v2(g_db,"SELECT item_name,price FROM canteen_items WHERE id=?",-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_int(st,1,atoi(iid)); if(sqlite3_step(st)==SQLITE_ROW){ strncpy(item,safe_text(st,0),sizeof(item)-1); price=sqlite3_column_double(st,1); }} sqlite3_finalize(st); if(sqlite3_prepare_v2(g_db,"INSERT INTO canteen_orders(customer_name,item_name,quantity,unit_price,total) VALUES(?,?,?,?,?)",-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,cid,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,item,-1,SQLITE_TRANSIENT); sqlite3_bind_int(st,3,qty); sqlite3_bind_double(st,4,price); sqlite3_bind_double(st,5,price*qty); sqlite3_step(st);} sqlite3_finalize(st); log_activity("canteen","Bill created",item); redirect_to(client,"/canteen",NULL); free(cid); free(iid); free(qtys); }

static void post_blood_add(socket_t client, const char *body){ char *sid=form_value(body,"student_id"),*name=form_value(body,"full_name"),*bg=form_value(body,"blood_group"),*phone=form_value(body,"phone"),*last=form_value(body,"last_donation"),*av=form_value(body,"availability"); sqlite3_stmt *st=NULL; const char *sql="INSERT INTO blood_donors(student_id,full_name,blood_group,phone,last_donation,availability,note) VALUES(?,?,?,?,?,?,?) ON CONFLICT(student_id) DO UPDATE SET full_name=excluded.full_name,blood_group=excluded.blood_group,phone=excluded.phone,last_donation=excluded.last_donation,availability=excluded.availability,note='Updated from portal'"; if(sqlite3_prepare_v2(g_db,sql,-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,sid,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,name,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,bg,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,phone,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,5,last,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,6,av,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,7,"Registered from portal",-1,SQLITE_TRANSIENT); sqlite3_step(st);} sqlite3_finalize(st); log_activity(name,"Blood donor saved",bg); redirect_to(client,"/blood-donation",NULL); free(sid); free(name); free(bg); free(phone); free(last); free(av); }

static void post_report_add(socket_t client, const char *body){ char *sid=form_value(body,"student_id"),*name=form_value(body,"student_name"),*type=form_value(body,"report_type"),*details=form_value(body,"details"); sqlite3_stmt *st=NULL; if(sqlite3_prepare_v2(g_db,"INSERT INTO student_reports(student_id,student_name,report_type,details,status) VALUES(?,?,?,?,?)",-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,sid,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,name,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,3,type,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,details,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,5,"Open",-1,SQLITE_TRANSIENT); sqlite3_step(st);} sqlite3_finalize(st); log_activity(name,"Student report submitted",type); add_mail_log("department@local","Student report submitted",details,"logged"); redirect_to(client,"/reports",NULL); free(sid); free(name); free(type); free(details); }

static void post_settings_save(socket_t client, const char *body){ char *host=form_value(body,"smtp_host"),*port=form_value(body,"smtp_port"),*user=form_value(body,"smtp_user"),*pass=form_value(body,"smtp_password"); sqlite3_stmt *st=NULL; const char *sql="INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value"; const char *keys[]={"smtp_host","smtp_port","smtp_user","smtp_password",NULL}; const char *vals[]={host,port,user,pass,NULL}; for(int i=0;keys[i];i++){ if(sqlite3_prepare_v2(g_db,sql,-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,keys[i],-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,vals[i],-1,SQLITE_TRANSIENT); sqlite3_step(st);} sqlite3_finalize(st); st=NULL;} log_activity("admin","Settings updated","SMTP settings saved."); redirect_to(client,"/settings",NULL); free(host); free(port); free(user); free(pass); }

static void post_password_change(socket_t client, const char *body){ char *old=form_value(body,"old_password"),*newp=form_value(body,"new_password"),*conf=form_value(body,"confirm_password"); if(strcmp(newp,conf)!=0){ char *p=page_password("New password and confirm password do not match."); send_raw(client,"200 OK","text/html",p,NULL); free(p); } else if(!valid_login("admin@pucse25.local",old)){ char *p=page_password("Current password is wrong."); send_raw(client,"200 OK","text/html",p,NULL); free(p); } else { char hash[32]; password_hash_hex(newp,hash); sqlite3_stmt *st=NULL; if(sqlite3_prepare_v2(g_db,"UPDATE users SET password_hash=? WHERE email='admin@pucse25.local'",-1,&st,NULL)==SQLITE_OK){ sqlite3_bind_text(st,1,hash,-1,SQLITE_TRANSIENT); sqlite3_step(st);} sqlite3_finalize(st); log_activity("admin","Password changed","Admin password updated."); add_mail_log("admin@local","Password updated","Admin password was updated.","logged"); char *p=page_password("Password updated successfully."); send_raw(client,"200 OK","text/html",p,NULL); free(p); } free(old); free(newp); free(conf); }

/* ------------------------- route dispatcher ------------------------- */
static void handle_request(socket_t client, char *req) {
    char method[8]="", url[512]="";
    sscanf(req, "%7s %511s", method, url);
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : "";
    char path[512]; strncpy(path,url,sizeof(path)-1); path[sizeof(path)-1]=0; char *q=strchr(path,'?'); if(q) *q=0;

    if (strcmp(path,"/")==0) { redirect_to(client,"/dashboard",NULL); return; }
    if (strcmp(path,"/style.css")==0) { char *css=load_file("public/css/style.css"); if(!css) css=load_file("../public/css/style.css"); if(css){ send_raw(client,"200 OK","text/css",css,NULL); free(css);} else send_raw(client,"404 Not Found","text/plain","CSS not found",NULL); return; }
    if (strncmp(path,"/images/",8)==0) { char fp[512]; snprintf(fp,sizeof(fp),"public%s",path); char *file=load_file(fp); if(!file){ snprintf(fp,sizeof(fp),"../public%s",path); file=load_file(fp); } const char *ct=strstr(path,".svg")?"image/svg+xml":(strstr(path,".png")?"image/png":(strstr(path,".jpg")||strstr(path,".jpeg")?"image/jpeg":"text/plain")); if(file){ send_raw(client,"200 OK",ct,file,NULL); free(file);} else send_raw(client,"404 Not Found","text/plain","Image not found",NULL); return; }
    if (strcmp(path,"/login")==0 && strcmp(method,"GET")==0) { char *p=page_login(NULL); send_raw(client,"200 OK","text/html",p,NULL); free(p); return; }
    if (strcmp(path,"/login")==0 && strcmp(method,"POST")==0) { post_login(client,body); return; }
    if (strcmp(path,"/register")==0 && strcmp(method,"GET")==0) { char *p=page_register(NULL); send_raw(client,"200 OK","text/html",p,NULL); free(p); return; }
    if (strcmp(path,"/register")==0 && strcmp(method,"POST")==0) { post_register(client,body); return; }
    if (strcmp(path,"/logout")==0) { redirect_to(client,"/login","Set-Cookie: pucse_session=; Path=/; Max-Age=0\r\n"); return; }

    if (!is_logged_in(req)) { redirect_to(client,"/login",NULL); return; }

    if (strcmp(method,"POST")==0) {
        if(strcmp(path,"/student/add")==0) post_student_add(client,body);
        else if(strcmp(path,"/attendance/add")==0) post_attendance_add(client,body);
        else if(strcmp(path,"/exam/add")==0) post_exam_add(client,body);
        else if(strcmp(path,"/routine/slot/add")==0) post_routine_slot_add(client,body);
        else if(strcmp(path,"/routine/image/save")==0) post_routine_image_save(client,body);
        else if(strcmp(path,"/result/publish")==0) post_result_publish(client,body);
        else if(strcmp(path,"/notice/add")==0) post_notice_add(client,body);
        else if(strcmp(path,"/group/join")==0) post_group_join(client,body);
        else if(strcmp(path,"/library/issue")==0) post_library_issue(client,body);
        else if(strcmp(path,"/canteen/order")==0) post_canteen_order(client,body);
        else if(strcmp(path,"/blood/add")==0) post_blood_add(client,body);
        else if(strcmp(path,"/report/add")==0) post_report_add(client,body);
        else if(strcmp(path,"/settings/save")==0) post_settings_save(client,body);
        else if(strcmp(path,"/password/change")==0) post_password_change(client,body);
        else redirect_to(client,"/dashboard",NULL);
        return;
    }

    char *page = NULL;
    if(strcmp(path,"/dashboard")==0) page=page_dashboard();
    else if(strcmp(path,"/department")==0) page=page_department();
    else if(strcmp(path,"/teachers")==0) page=page_teachers();
    else if(strcmp(path,"/students")==0) page=page_students();
    else if(strcmp(path,"/courses")==0) page=page_courses();
    else if(strcmp(path,"/routine")==0) page=page_routine();
    else if(strcmp(path,"/results")==0) page=page_results(NULL);
    else if(strcmp(path,"/attendance")==0) page=page_attendance();
    else if(strcmp(path,"/ct-exams")==0) page=page_ct_exams();
    else if(strcmp(path,"/notices")==0) page=page_notices();
    else if(strcmp(path,"/groups")==0) page=page_groups();
    else if(strcmp(path,"/library")==0) page=page_library();
    else if(strcmp(path,"/canteen")==0) page=page_canteen();
    else if(strcmp(path,"/blood-donation")==0) page=page_blood();
    else if(strcmp(path,"/labs")==0) page=page_labs();
    else if(strcmp(path,"/reports")==0) page=page_reports();
    else if(strcmp(path,"/activity")==0) page=page_activity();
    else if(strcmp(path,"/settings")==0) page=page_settings();
    else if(strcmp(path,"/password")==0) page=page_password(NULL);
    else page=page_not_found();
    send_raw(client,"200 OK","text/html",page,NULL);
    free(page);
}


static char *case_find(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return (char*)haystack;
    size_t nl = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return (char*)p;
    }
    return NULL;
}

static int read_http_request(socket_t client, char **out) {
    char *buf = (char*)malloc(MAX_BODY + BUF_RECV + 1);
    int total = 0;
    int n;
    while ((n = recv(client, buf + total, BUF_RECV, 0)) > 0) {
        total += n;
        buf[total] = 0;
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            int content_length = 0;
            char *cl = case_find(buf, "Content-Length:");
            if (cl) content_length = atoi(cl + 15);
            int header_size = (int)(hdr_end + 4 - buf);
            if (total >= header_size + content_length) break;
        }
        if (total > MAX_BODY) break;
    }
    if (total <= 0) { free(buf); return 0; }
    *out = buf; return 1;
}

int main(void) {
    const char *env_port = getenv("PORT");
    if (env_port && atoi(env_port) > 0) g_port = atoi(env_port);
    if (sqlite3_open(DB_FILE, &g_db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(g_db));
        return 1;
    }
    create_schema();
    seed_database();

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { fprintf(stderr, "WSAStartup failed\n"); return 1; }
#endif

    socket_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) { fprintf(stderr, "Socket creation failed\n"); return 1; }

    int opt = 1;
#ifdef _WIN32
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(g_port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Bind failed. Port %d may already be used. You can set PORT=8081 and run again.\n", g_port);
        CLOSESOCKET(server_fd);
        return 1;
    }
    if (listen(server_fd, 20) == SOCKET_ERROR) {
        fprintf(stderr, "Listen failed\n"); CLOSESOCKET(server_fd); return 1;
    }

    printf("Pundra CSE Smart Portal 25 is running.\n");
    printf("Open: http://localhost:%d\n", g_port);
    printf("Database file: %s\n", DB_FILE);
    printf("Press Ctrl+C to stop server.\n");

    while (1) {
        struct sockaddr_in client_addr;
#ifdef _WIN32
        int client_len = sizeof(client_addr);
#else
        socklen_t client_len = sizeof(client_addr);
#endif
        socket_t client = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client == INVALID_SOCKET) continue;
        char *request = NULL;
        if (read_http_request(client, &request)) {
            handle_request(client, request);
            free(request);
        }
        CLOSESOCKET(client);
    }

    CLOSESOCKET(server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    sqlite3_close(g_db);
    return 0;
}
