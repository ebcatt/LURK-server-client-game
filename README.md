# LURK Server/Client Game

## What this is

This project is a server/client game built in C++ using the LURK protocol. The main goal was to create a working multiplayer-style setup where a client and server could communicate over a network connection using the required message format.

## Why I built it

I built this project as part of coursework focused on systems and network programming. It gave me the chance to work on something lower-level than a normal web project and pushed me to think more about sockets, communication, and protocol structure.

## What it does

* Sets up a client/server connection over a port
* Uses the LURK protocol for communication
* Sends and receives structured messages between the server and client
* Runs in a Linux/server environment instead of just locally in a browser

## Tech used

* C++
* Socket programming
* LURK protocol
* Linux environment / server hosting
* Isoptera server for testing and running

## How to run it

**CLIENT FILE WILL BE ADDED LATER

This project was developed and tested in a Linux-based environment and was run through a port on the Isoptera server.

General setup:

1. Clone the repository
2. Compile the server and client files
3. Start the server on an available port
4. Connect the client to that port
5. Interact through the LURK protocol
     Find here: https://isoptera.lcsc.edu/~seth/cs435/lurk_2.3.html

Because this project depends on a Linux environment, sockets, and protocol-specific communication, it may not run correctly without the same setup.

## Project status

This project is functional in the sense that it did run, but it still has bugs and unfinished pieces. I’m including it because it reflects the systems programming and networking work that went into building it, not because it is a perfectly polished final product.

## What I learned

This project helped me learn a lot about:

* how client/server communication actually works
* working with sockets and ports
* following a required communication protocol
* debugging lower-level issues that are a lot less forgiving than front-end projects
* building and testing code in a Linux environment

## Challenges

Honestly, this project was frustrating at times. Debugging protocol and socket issues is a lot harder when things break silently or don’t behave the way you expect. It also took effort to get the project running correctly through the server environment and make sure the communication matched the LURK protocol requirements.

## If I kept working on it

I’d like to:

* fix the remaining bugs
* improve room and player handling
* make the gameplay flow more stable
* clean up the overall code structure
* add more complete game logic

## Notes

This project is one of the more technical things I’ve worked on so far. Even though it still has issues, it shows experience with networking, protocol-based communication, and Linux/server-side development in C++.
