{
  lib,
  noctaliaPackage,
}:

noctaliaPackage.overrideAttrs (oldAttrs: {
  pname = "noctalia-greetd";
  version = "0.2.0";
  __intentionallyOverridingVersion = true;
  mesonFlags = (oldAttrs.mesonFlags or [ ]) ++ [ "-Dtests=disabled" ];

  postPatch = ''
        ${oldAttrs.postPatch or ""}
        cp ${../src/main.cpp} src/main.cpp
        cp ${../src/appearance.h} src/appearance.h
        cp ${../src/lock_widgets_scene.h} src/lock_widgets_scene.h
        cp ${../src/lock_widget_services.h} src/lock_widget_services.h
    cp ${../tests/appearance_test.cpp} tests/greetd_appearance_test.cpp
    cp ${../tests/lock_widgets_scene_test.cpp} tests/greetd_lock_widgets_scene_test.cpp
        cp ${../src/session_lock_surface.h} src/session_lock_surface.h
        cp ${../src/on_screen_keyboard.h} src/on_screen_keyboard.h
        cp ${../src/desktop_keyboard.h} src/desktop_keyboard.h
        cp ${../src/controller_pointer.h} src/controller_pointer.h
        cp ${../src/session_lock_hint.h} src/session_lock_hint.h
        cp ${../protocols/wlr-virtual-pointer-unstable-v1.xml} protocols/wlr-virtual-pointer-unstable-v1.xml
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
        pointer_protocol_snippet="$(cat <<'MESON'
    _virtual_pointer_xml = 'protocols/wlr-virtual-pointer-unstable-v1.xml'
    _virtual_pointer_h = custom_target('virtual-pointer-client-header',
      input: _virtual_pointer_xml,
      output: 'wlr-virtual-pointer-unstable-v1-client-protocol.h',
      command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
    )
    _virtual_pointer_c = custom_target('virtual-pointer-client-source',
      input: _virtual_pointer_xml,
      output: 'wlr-virtual-pointer-unstable-v1-client-protocol.c',
      command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
      depends: _virtual_pointer_h,
    )
    _protocol_sources += _virtual_pointer_c
    _protocol_headers += _virtual_pointer_h
    MESON
        )"
        ksld_protocol_snippet="$ksld_protocol_snippet
    $pointer_protocol_snippet"
        substituteInPlace meson.build \
          --replace-fail \
          "_protocol_headers += _wlr_ls_h" \
          "$ksld_protocol_snippet"
        substituteInPlace meson.build \
          --replace-fail "executable('noctalia'," "executable('noctalia-greetd',"
    cat >> meson.build <<'MESON'
    greetd_appearance_test = executable('greetd_appearance_test',
      sources: files('tests/greetd_appearance_test.cpp'),
      dependencies: noctalia_core_dep,
      override_options: ['b_ndebug=false'],
    )
    test('greetd_appearance', greetd_appearance_test)
    greetd_lock_widgets_scene_test = executable('greetd_lock_widgets_scene_test',
      sources: files('tests/greetd_lock_widgets_scene_test.cpp'),
      dependencies: noctalia_core_dep,
      override_options: ['b_ndebug=false'],
    )
    test('greetd_lock_widgets_scene', greetd_lock_widgets_scene_test)
    MESON
  '';

  postFixup =
    let
      renamedPostFixup = builtins.replaceStrings [ "$out/bin/noctalia" ] [ "$out/bin/noctalia-greetd" ] (
        oldAttrs.postFixup or ""
      );
    in
    lib.concatStringsSep "\n" (
      lib.filter (line: !(lib.hasInfix " completions " line) && !(lib.hasInfix "share-picker" line)) (
        lib.splitString "\n" renamedPostFixup
      )
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
