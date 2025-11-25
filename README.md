# SyncText:- CRDT based text file synchronization
This project uses CRDT mechanisms achieve synchronization across multiple users editing a common text document. Unlike lock-based mechanisms, it does not use any locks on a global document to achieve synchronization making it extremely scalable. 

## Compiling:- 
gcc synctext.c -o synctext -lrt -lpthread

## Executing:- 
### Execute the following command on separate terminals to initiate different users:- 
./synctext username
