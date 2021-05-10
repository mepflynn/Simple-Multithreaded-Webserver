This repository largely includes the provided code for the simple HTTP web server.

The main edits that I made were in the gunrock_web.cpp source file, wherein I spawned consumer threads to receive HTTP request jobs from the main producer thread.

The provided code is relatively lacking in comments, and so it is easy to pick out my work by looking for large blocks of commented code.