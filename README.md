# Pundra CSE Smart Portal 25

A Windows/MSYS2 UCRT64 university project using **C backend + SQLite3 database + HTML/CSS frontend**.

## v5 Final Updates

- Department page has a colorful photo board with pictures loaded from Pundra University web resources.
- Course page is fully colorful with gradient course cards; no plain white-only course UI.
- Routine page added.
  - Default CSE 25 Batch routine image included: `public/images/cse25_routine.svg`
  - Routine image/path can be changed from the portal.
  - Routine slots can be added/updated from the portal.
- Result Publish page added.
  - Select student.
  - Select course.
  - Enter marks.
  - Backend calculates letter grade, grade point and credit-wise CGPA.
- Blood Donation still prevents duplicate Student ID.
- Registration is active immediately; admin approval is not required.
- Login and registration include password Show/Hide option.
- Admin credential is not shown on login page.

## Project modules

- Dashboard with update feed
- Department page using Pundra CSE information and web photos
- CSE teacher list with official faculty photos loaded from Pundra University URLs
- CSE 25 Batch 4th Semester student list
- Course page with colorful course cards and teacher mapping
- Routine page with default editable routine image and routine slot table
- Course-wise attendance update by date
- CT & Exams page with lab final, CT, assignment and lab report defaults
- Result Publish and CGPA calculator page
- Notice board for Department, Teacher, CR and Admin
- Groups: Data Research, Website Project, Robotics, Software Development, Programming Practice
- Library book copy, issue/request records and mail log
- Canteen item price and bill calculation
- Blood Donation registration and default donor data
- Lab Management with CSE lab rooms from routine/facility context
- Student Reports
- Activity logs
- Settings and password update

## Windows setup with MSYS2 UCRT64

1. Install MSYS2 from https://www.msys2.org/
2. Open **MSYS2 UCRT64** from Start Menu.
3. Install compiler and SQLite3:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sqlite3
```

4. Go to the extracted project folder. Example:

```bash
cd "/c/C Project For Uni/pundra_cse_smart_portal_25_v5"
```

5. Compile:

```bash
gcc src/main.c -o PundraCSESmartPortal25.exe -lsqlite3 -lws2_32
```

6. Run:

```bash
./PundraCSESmartPortal25.exe
```

7. Open browser:

```text
http://localhost:8080
```

If port 8080 is busy:

```bash
PORT=8081 ./PundraCSESmartPortal25.exe
```

Then open:

```text
http://localhost:8081
```

## Default admin login

The login page does not show the admin credential.

```text
Email: admin@pucse25.local
Password: Pucse@2025
```

After login, go to **Password** page and change it.

## Database location

SQLite database file is included and also auto-created/updated when the server runs:

```text
pundra_cse_smart_portal_25.db
```

Schema file:

```text
database/schema.sql
```

## Routine image change

Default routine image:

```text
public/images/cse25_routine.svg
```

To use your own routine image:

1. Copy your image into `public/images/`.
2. Example filename: `my_routine.png`.
3. Go to **Routine** page.
4. Set image path:

```text
/images/my_routine.png
```

5. Save routine image.

## CGPA grade scale

```text
80-100 = A+ = 4.00
75-79  = A  = 3.75
70-74  = A- = 3.50
65-69  = B+ = 3.25
60-64  = B  = 3.00
55-59  = B- = 2.75
50-54  = C+ = 2.50
45-49  = C  = 2.25
40-44  = D  = 2.00
Below 40 = F = 0.00
```

## Important note about email

This version saves email alerts into `mail_logs` table. Real SMTP sending needs your SMTP/API credential. The Settings page and database fields are ready for SMTP/API configuration.

## JavaScript note

The portal does not use external JavaScript files. Only a tiny inline password Show/Hide button is used because you requested password show/hide on login/registration.
