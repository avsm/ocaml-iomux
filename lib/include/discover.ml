module C = Configurator.V1

let has_ppoll_code = {|
#define _GNU_SOURCE /* for linux */
#include <poll.h>
#include <stddef.h>
#include <strings.h>

int
main(void)
{
	struct pollfd fds;
	struct timespec ts;

	bzero(&fds, sizeof(fds));
	bzero(&ts, sizeof(ts));

	return (ppoll(&fds, 0, &ts, NULL));
}
|}

let is_windows_code = {|
#ifdef _WIN32
int main(void) { return 0; }
#else
#error "not windows"
#endif
|}

let has_wspoll_code = {|
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

int main(void)
{
	WSAPOLLFD fds;
	fds.fd = INVALID_SOCKET;
	fds.events = 0;
	fds.revents = 0;
	return WSAPoll(&fds, 0, 0);
}
#else
#error "not windows"
#endif
|}

let () =
  C.main ~name:"discover" @@ fun c ->

  (* check if we're on Windows *)
  let is_windows = C.c_test c is_windows_code in

  (* check for ppoll(2) on Unix, WSAPoll on Windows *)
  let has_ppoll = if is_windows then false else C.c_test c has_ppoll_code in
  let has_wspoll = if is_windows then C.c_test c has_wspoll_code else false in

  C.C_define.gen_header_file c ~fname:"config.h" [
    "HAS_PPOLL", Switch has_ppoll;
    "IS_WINDOWS", Switch is_windows;
    "HAS_WSPOLL", Switch has_wspoll;
  ];

  let has_list = [
    Printf.sprintf "let has_ppoll = %b" has_ppoll;
    Printf.sprintf "let is_windows = %b" is_windows;
  ] in

  (* general poll(2) definitions *)
  let includes = if is_windows then ["winsock2.h"; "ws2tcpip.h"] else ["poll.h"] in
  let struct_name = if is_windows then "WSAPOLLFD" else "struct pollfd" in

  let defs =
    C.C_define.import c ~includes
      C.C_define.Type.[
        "POLLIN", Int;
        "POLLPRI", Int;
        "POLLOUT", Int;
        "POLLERR", Int;
        "POLLHUP", Int;
        "POLLNVAL", Int;
        Printf.sprintf "sizeof(%s)" struct_name, Int;
      ]
    |> List.map (function
        | name, C.C_define.Value.Int v ->
          let name =
            match name with
            | s when String.starts_with ~prefix:"sizeof(" s -> "sizeof_pollfd"
            | nm -> nm
          in
          Printf.sprintf "let %s = 0x%x" (String.lowercase_ascii name) v
        | _ -> assert false
      )
  in
  C.Flags.write_lines "config.ml" (defs @ has_list);

  (* Generate c_library_flags.sexp for Windows linking *)
  let c_library_flags =
    if is_windows then
      ["-lws2_32"]
    else
      []
  in
  C.Flags.write_sexp "c_library_flags.sexp" c_library_flags
