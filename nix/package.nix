{
  lib,
  noctaliaPackage,
}:

noctaliaPackage.overrideAttrs (oldAttrs: {
  pname = "noctalia-greetd";
  version = "0.1.0";
  __intentionallyOverridingVersion = true;
  mesonFlags = (oldAttrs.mesonFlags or [ ]) ++ [ "-Dtests=disabled" ];

  postPatch = ''
    ${oldAttrs.postPatch or ""}
    cp ${../src/main.cpp} src/main.cpp
    cp ${../src/lock_visual_layout.h} src/shell/lockscreen/lock_visual_layout.h
    cp ${../src/lock_visual_layout.cpp} src/shell/lockscreen/lock_visual_layout.cpp
    cp ${../protocols/ksld.xml} protocols/ksld.xml
    ksld_protocol_snippet="$(printf '%s\n' \
      "_protocol_headers += _wlr_ls_h" \
      "" \
      "_ksld_xml = 'protocols/ksld.xml'" \
      "_ksld_h = custom_target('ksld-client-header'," \
      "  input: _ksld_xml," \
      "  output: 'ksld-client-protocol.h'," \
      "  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']," \
      ")" \
      "_ksld_c = custom_target('ksld-client-source'," \
      "  input: _ksld_xml," \
      "  output: 'ksld-client-protocol.c'," \
      "  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']," \
      "  depends: _ksld_h," \
      ")" \
      "_protocol_sources += _ksld_c" \
      "_protocol_headers += _ksld_h")"
    substituteInPlace meson.build \
      --replace-fail \
      "_protocol_headers += _wlr_ls_h" \
      "$ksld_protocol_snippet"
    substituteInPlace meson.build \
      --replace-fail "executable('noctalia'," "executable('noctalia-greetd',"
  '';

  postFixup =
    let
      renamedPostFixup = builtins.replaceStrings [ "$out/bin/noctalia" ] [ "$out/bin/noctalia-greetd" ] (
        oldAttrs.postFixup or ""
      );
    in
    lib.concatStringsSep "\n" (
      lib.filter (line: !(lib.hasInfix " completions " line)) (lib.splitString "\n" renamedPostFixup)
    );

  patches = (oldAttrs.patches or [ ]) ++ [
    ../patches/lockscreen-shared-visual-layout.patch
  ];

  meta = (oldAttrs.meta or { }) // {
    description = "Native Noctalia-style greetd greeter";
    homepage = "https://github.com/Zvampen04/noctalia-greetd";
    license = lib.licenses.mit;
    mainProgram = "noctalia-greetd";
  };
})
