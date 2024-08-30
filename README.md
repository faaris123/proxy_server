# Proxy Server
I created a proxy web server using HTTP, which is the most commonly used application protocol on the Internet today. This also uses a client-server mode that will open a connection to a server and send a request. The server will then respond with the appropriate message.
# Details
I implemented a proxy server that handles GET requests. Through the use of HTTP response headers, I added support for HTTP error codes and passed the request to an HTTP file server. The server will then wait for the response from the file server and forward the response. I also implemented a Priority Queue for the jobs sent to the proxy server, which will implement functionality that allows the client to query it for job status and order.
# Included
I have included all the C files needed to run the application
