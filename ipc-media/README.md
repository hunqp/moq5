# IP Camera Web Engine

A browser-based interface for monitoring and managing IP cameras. The system connects the user interface to camera services through a secure web server and a CGI/FastCGI application gateway.

## Main capabilities

- Live video monitoring and snapshots.
- Recorded video playback and downloads.
- Video, audio, network and storage configuration.
- Event, GPIO and industrial interface management.
- Device activation, user authentication and role-based access.

## Design overview

The frontend presents camera status and user controls. The CGI/FastCGI gateway processes authorized requests and coordinates backend camera services. A separate media path delivers live video to the browser.

See [System Architecture](ARCHITECTURE.md) for the component diagram and operating flows.
