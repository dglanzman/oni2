open Oni_Core;

// MODEL

type terminal = {
  id: int,
  launchConfig: Exthost.ShellLaunchConfig.t,
  rows: int,
  columns: int,
  pid: option(int),
  title: option(string),
  screen: ReveryTerminal.Screen.t,
  cursor: ReveryTerminal.Cursor.t,
  closeOnExit: bool,
};

type t = {
  idToTerminal: IntMap.t(terminal),
  nextId: int,
};

let initial = {idToTerminal: IntMap.empty, nextId: 0};

let getBufferName = (id, cmd) =>
  Printf.sprintf("oni://terminal/%d/%s", id, cmd);

let toList = ({idToTerminal, _}) =>
  idToTerminal |> IntMap.bindings |> List.map(snd);

let getTerminalOpt = (id, {idToTerminal, _}) =>
  IntMap.find_opt(id, idToTerminal);

// UPDATE

[@deriving show({with_path: false})]
type splitDirection =
  | Vertical
  | Horizontal
  | Current;

[@deriving show({with_path: false})]
type command =
  | NewTerminal({
      cmd: option(string),
      splitDirection,
      closeOnExit: bool,
    })
  | NormalMode
  | InsertMode;

[@deriving show({with_path: false})]
type msg =
  | Command(command)
  | Resized({
      id: int,
      rows: int,
      columns: int,
    })
  | KeyPressed({
      id: int,
      key: string,
    })
  | Pasted({
      id: int,
      text: string,
    })
  | Service(Service_Terminal.msg);

type outmsg =
  | Nothing
  | Effect(Isolinear.Effect.t(msg))
  | TerminalCreated({
      name: string,
      splitDirection,
    })
  | TerminalExit({
      terminalId: int,
      exitCode: int,
      shouldClose: bool,
    });

let shellCmd = ShellUtility.getDefaultShell();

// CONFIGURATION

module Configuration = {
  open Oni_Core;
  open Config.Schema;
  module Codecs = Feature_Configuration.GlobalConfiguration.Codecs;

  module Shell = {
    let windows =
      setting("terminal.integrated.shell.windows", string, ~default=shellCmd);
    let linux =
      setting("terminal.integrated.shell.linux", string, ~default=shellCmd);
    let osx =
      setting("terminal.integrated.shell.osx", string, ~default=shellCmd);
  };

  module ShellArgs = {
    let windows =
      setting(
        "terminal.integrated.shellArgs.windows",
        list(string),
        ~default=[],
      );
    let linux =
      setting(
        "terminal.integrated.shellArgs.linux",
        list(string),
        ~default=[],
      );
    let osx =
      setting(
        "terminal.integrated.shellArgs.osx",
        list(string),
        // ~/.[bash|zsh}_profile etc is not sourced when logging in on macOS.
        // Instead, terminals on macOS should run as a login shell (which in turn
        // sources these files).
        // See more at http://unix.stackexchange.com/a/119675/115410.
        ~default=["-l"],
      );
  };

  let fontFamily =
    setting(
      "terminal.integrated.fontFamily",
      nullable(string),
      ~default=None,
    );

  let fontSize =
    setting(
      "terminal.integrated.fontSize",
      nullable(Codecs.fontSize),
      ~default=None,
    );
  let fontWeight =
    setting(
      "terminal.integrated.fontWeight",
      nullable(Codecs.fontWeight),
      ~default=None,
    );

  let fontLigatures =
    setting(
      "terminal.integrated.fontLigatures",
      nullable(Codecs.fontLigatures),
      ~default=None,
    );

  let fontSmoothing =
    setting(
      "terminal.integrated.fontSmoothing",
      nullable(
        custom(~encode=FontSmoothing.encode, ~decode=FontSmoothing.decode),
      ),
      ~default=None,
    );
};

let shouldClose = (~id, {idToTerminal, _}) => {
  IntMap.find_opt(id, idToTerminal)
  |> Option.map(({closeOnExit, _}) => closeOnExit)
  |> Option.value(~default=true);
};

let updateById = (id, f, model) => {
  let idToTerminal = IntMap.update(id, Option.map(f), model.idToTerminal);
  {...model, idToTerminal};
};

let update =
    (
      ~clientServer: Feature_ClientServer.model,
      ~config: Config.resolver,
      model: t,
      msg,
    ) => {
  switch (msg) {
  | Command(NewTerminal({cmd, splitDirection, closeOnExit})) =>
    let cmdToUse =
      switch (cmd) {
      | None =>
        switch (Revery.Environment.os) {
        | Windows(_) => Configuration.Shell.windows.get(config)
        | Mac(_) => Configuration.Shell.osx.get(config)
        | Linux(_) => Configuration.Shell.linux.get(config)
        | _ => shellCmd
        }
      | Some(specifiedCommand) => specifiedCommand
      };

    let arguments =
      switch (Revery.Environment.os) {
      | Windows(_) => Configuration.ShellArgs.windows.get(config)
      | Mac(_) => Configuration.ShellArgs.osx.get(config)
      | Linux(_) => Configuration.ShellArgs.linux.get(config)
      | _ => []
      };

    let defaultEnvVariables =
      [
        ("ONIVIM_TERMINAL", BuildInfo.version),
        (
          "ONIVIM2_PARENT_PIPE",
          clientServer
          |> Feature_ClientServer.pipe
          |> Exthost.NamedPipe.toString,
        ),
      ]
      |> List.to_seq
      |> StringMap.of_seq;

    let env =
      Exthost.ShellLaunchConfig.(
        switch (Revery.Environment.os) {
        // Windows - simply inherit from the running process
        | Windows(_) => Additive(defaultEnvVariables)

        // Mac - inherit (we rely on the '-l' flag to pick up user config)
        | Mac(_) => Additive(defaultEnvVariables)

        // For Linux, there's a few stray variables that may come in from the AppImage
        // for example - LD_LIBRARY_PATH in issue #2040. We need to clear those out.
        | Linux(_) =>
          switch (
            Sys.getenv_opt("ONI2_ORIG_PATH"),
            Sys.getenv_opt("ONI2_ORIG_LD_LIBRARY_PATH"),
          ) {
          // We're running from the AppImage, which tracks the original env.
          | (Some(origPath), Some(origLdLibPath)) =>
            let envVariables =
              [("PATH", origPath), ("LD_LIBRARY_PATH", origLdLibPath)]
              |> List.to_seq
              |> StringMap.of_seq;

            let merge = (maybeA, maybeB) =>
              switch (maybeA, maybeB) {
              | (Some(_) as a, Some(_)) => a
              | (Some(_) as a, _) => a
              | (None, Some(_) as b) => b
              | (None, None) => None
              };
            let allVariables =
              StringMap.merge(
                _key => merge,
                envVariables,
                defaultEnvVariables,
              );

            Additive(allVariables);

          // All other cases - just inherit. Maybe not running from AppImage.
          | _ => Additive(defaultEnvVariables)
          }

        | _ => Additive(defaultEnvVariables)
        }
      );

    let launchConfig =
      Exthost.ShellLaunchConfig.{
        name: "Terminal",
        arguments,
        executable: cmdToUse,
        env,
      };

    let id = model.nextId;
    let idToTerminal =
      IntMap.add(
        id,
        {
          id,
          launchConfig,
          rows: 40,
          columns: 40,
          pid: None,
          title: None,
          screen: ReveryTerminal.Screen.initial,
          cursor: ReveryTerminal.Cursor.initial,
          closeOnExit,
        },
        model.idToTerminal,
      );
    (
      {idToTerminal, nextId: id + 1},
      TerminalCreated({name: getBufferName(id, cmdToUse), splitDirection}),
    );

  | Command(InsertMode | NormalMode) =>
    // Used for the renderer state
    (model, Nothing)

  | KeyPressed({id, key}) =>
    let inputEffect =
      Service_Terminal.Effect.input(~id, key)
      |> Isolinear.Effect.map(msg => Service(msg));
    (model, Effect(inputEffect));

  | Pasted({id, text}) =>
    let inputEffect =
      Service_Terminal.Effect.paste(~id, text)
      |> Isolinear.Effect.map(msg => Service(msg));
    (model, Effect(inputEffect));

  | Resized({id, rows, columns}) =>
    let newModel = updateById(id, term => {...term, rows, columns}, model);
    (newModel, Nothing);

  | Service(ProcessStarted({id, pid})) =>
    let newModel = updateById(id, term => {...term, pid: Some(pid)}, model);
    (newModel, Nothing);

  | Service(ProcessTitleChanged({id, title})) =>
    let newModel =
      updateById(id, term => {...term, title: Some(title)}, model);
    (newModel, Nothing);

  | Service(ScreenUpdated({id, screen, cursor})) =>
    let newModel = updateById(id, term => {...term, screen, cursor}, model);
    (newModel, Nothing);

  | Service(ProcessExit({id, exitCode})) => (
      model,
      TerminalExit({
        terminalId: id,
        exitCode,
        shouldClose: shouldClose(~id, model),
      }),
    )
  };
};

let subscription = (~setup, ~workspaceUri, extHostClient, model: t) => {
  model
  |> toList
  |> List.map((terminal: terminal) => {
       Service_Terminal.Sub.terminal(
         ~setup,
         ~id=terminal.id,
         ~launchConfig=terminal.launchConfig,
         ~rows=terminal.rows,
         ~columns=terminal.columns,
         ~workspaceUri,
         ~extHostClient,
       )
     })
  |> Isolinear.Sub.batch
  |> Isolinear.Sub.map(msg => Service(msg));
};

// COLORS
module Colors = Feature_Theme.Colors.Terminal;

let theme = theme =>
  fun
  | 0 => Colors.ansiBlack.from(theme)
  | 1 => Colors.ansiRed.from(theme)
  | 2 => Colors.ansiGreen.from(theme)
  | 3 => Colors.ansiYellow.from(theme)
  | 4 => Colors.ansiBlue.from(theme)
  | 5 => Colors.ansiMagenta.from(theme)
  | 6 => Colors.ansiCyan.from(theme)
  | 7 => Colors.ansiWhite.from(theme)
  | 8 => Colors.ansiBrightBlack.from(theme)
  | 9 => Colors.ansiBrightRed.from(theme)
  | 10 => Colors.ansiBrightGreen.from(theme)
  | 11 => Colors.ansiBrightYellow.from(theme)
  | 12 => Colors.ansiBrightBlue.from(theme)
  | 13 => Colors.ansiBrightMagenta.from(theme)
  | 14 => Colors.ansiBrightCyan.from(theme)
  | 15 => Colors.ansiBrightWhite.from(theme)
  // For 256 colors, fall back to defaults
  | idx => ReveryTerminal.Theme.default(idx);

let defaultBackground = theme => Colors.background.from(theme);
let defaultForeground = theme => Colors.foreground.from(theme);

let getFirstNonEmptyLine =
    (~start: int, ~direction: int, lines: array(string)) => {
  let len = Array.length(lines);

  let rec loop = idx =>
    if (idx == len || idx < 0) {
      idx;
    } else if (String.length(lines[idx]) == 0) {
      loop(idx + direction);
    } else {
      idx;
    };

  loop(start);
};

let getFirstNonEmptyLineFromTop = (lines: array(string)) => {
  getFirstNonEmptyLine(~start=0, ~direction=1, lines);
};

let getFirstNonEmptyLineFromBottom = (lines: array(string)) => {
  getFirstNonEmptyLine(~start=Array.length(lines) - 1, ~direction=-1, lines);
};

type highlights = (int, list(ThemeToken.t));

module TermScreen = ReveryTerminal.Screen;

let addHighlightForCell =
    (~defaultBackground, ~defaultForeground, ~theme, ~cell, ~column, tokens) => {
  let fg =
    TermScreen.getForegroundColor(
      ~defaultBackground,
      ~defaultForeground,
      ~theme,
      cell,
    );
  let bg =
    TermScreen.getBackgroundColor(
      ~defaultBackground,
      ~defaultForeground,
      ~theme,
      cell,
    );

  // TODO: Hook up the bold/italic in revery-terminal
  let newToken =
    ThemeToken.{
      index: column,
      backgroundColor: bg,
      foregroundColor: fg,
      syntaxScope: SyntaxScope.none,
      bold: false,
      italic: false,
    };

  switch (tokens) {
  | [ThemeToken.{foregroundColor, backgroundColor, _} as ct, ...tail]
      when foregroundColor != fg && backgroundColor != bg => [
      newToken,
      ct,
      ...tail,
    ]
  | [] => [newToken]
  | list => list
  };
};

let getLinesAndHighlights = (~colorTheme, ~terminalId) => {
  terminalId
  |> Service_Terminal.getScreen
  |> Option.map(screen => {
       module TermScreen = ReveryTerminal.Screen;
       let totalRows = TermScreen.getTotalRows(screen);
       let columns = TermScreen.getColumns(screen);

       let lines = Array.make(totalRows, "");

       let highlights = ref([]);
       let theme = theme(colorTheme);
       let defaultBackground = defaultBackground(colorTheme);
       let defaultForeground = defaultForeground(colorTheme);

       for (lineIndex in 0 to totalRows - 1) {
         let buffer = Stdlib.Buffer.create(columns * 2);
         let lineHighlights = ref([]);
         for (column in 0 to columns - 1) {
           let cell = TermScreen.getCell(~row=lineIndex, ~column, screen);
           let codeInt = Uchar.to_int(cell.char);
           if (codeInt != 0 && codeInt <= 0x10FFFF) {
             Stdlib.Buffer.add_utf_8_uchar(buffer, cell.char);

             lineHighlights :=
               addHighlightForCell(
                 ~defaultBackground,
                 ~defaultForeground,
                 ~theme,
                 ~cell,
                 ~column,
                 lineHighlights^,
               );
           } else {
             Stdlib.Buffer.add_string(buffer, " ");
           };
         };

         let str =
           Stdlib.Buffer.contents(buffer) |> Utility.StringEx.trimRight;
         highlights :=
           [(lineIndex, lineHighlights^ |> List.rev), ...highlights^];
         lines[lineIndex] = str;
       };

       let startLine = getFirstNonEmptyLineFromTop(lines);
       let bottomLine = getFirstNonEmptyLineFromBottom(lines);

       let lines =
         Utility.ArrayEx.slice(
           ~start=startLine,
           ~length=bottomLine - startLine + 1,
           ~lines,
           (),
         );

       let highlights =
         highlights^
         |> List.map(((idx, tokens)) => (idx - startLine, tokens));

       (lines, highlights);
     })
  |> Option.value(~default=([||], []));
};

// BUFFERRENDERER

// TODO: Unify the model and renderer state. This shouldn't be needed.
[@deriving show({with_path: false})]
type rendererState = {
  title: string,
  id: int,
  insertMode: bool,
};

let bufferRendererReducer = (state, action) => {
  switch (action) {
  | Service(ProcessTitleChanged({id, title, _})) when state.id == id => {
      ...state,
      id,
      title,
    }
  | Command(NormalMode) => {...state, insertMode: false}
  | Command(InsertMode) => {...state, insertMode: true}

  | _ => state
  };
};

// COMMANDS

module Commands = {
  open Feature_Commands.Schema;

  module New = {
    let horizontal =
      define(
        ~category="Terminal",
        ~title="Open terminal in new horizontal split",
        "terminal.new.horizontal",
        Command(
          NewTerminal({
            cmd: None,
            splitDirection: Horizontal,
            closeOnExit: true,
          }),
        ),
      );
    let vertical =
      define(
        ~category="Terminal",
        ~title="Open terminal in new vertical split",
        "terminal.new.vertical",
        Command(
          NewTerminal({
            cmd: None,
            splitDirection: Vertical,
            closeOnExit: true,
          }),
        ),
      );
    let current =
      define(
        ~category="Terminal",
        ~title="Open terminal in current window",
        "terminal.new.current",
        Command(
          NewTerminal({
            cmd: None,
            splitDirection: Current,
            closeOnExit: true,
          }),
        ),
      );
  };

  module Oni = {
    let normalMode = define("oni.terminal.normalMode", Command(NormalMode));
    let insertMode = define("oni.terminal.insertMode", Command(InsertMode));
  };
};

// CONTRIBUTIONS

module Contributions = {
  let colors =
    Colors.[
      background,
      foreground,
      ansiBlack,
      ansiRed,
      ansiGreen,
      ansiYellow,
      ansiBlue,
      ansiMagenta,
      ansiCyan,
      ansiWhite,
      ansiBrightBlack,
      ansiBrightRed,
      ansiBrightGreen,
      ansiBrightYellow,
      ansiBrightBlue,
      ansiBrightCyan,
      ansiBrightMagenta,
      ansiBrightWhite,
    ];

  let commands =
    Commands.[
      New.horizontal,
      New.vertical,
      New.current,
      Oni.normalMode,
      Oni.insertMode,
    ];

  let configuration =
    Configuration.[
      Shell.windows.spec,
      Shell.linux.spec,
      Shell.osx.spec,
      ShellArgs.windows.spec,
      ShellArgs.linux.spec,
      ShellArgs.osx.spec,
      fontFamily.spec,
      fontSize.spec,
      fontWeight.spec,
      fontLigatures.spec,
      fontSmoothing.spec,
    ];

  let keybindings = {
    Feature_Input.Schema.[
      // Insert mode -> normal mdoe
      bind(
        ~key="<C-\\><C-N>",
        ~command=Commands.Oni.normalMode.id,
        ~condition="terminalFocus && insertMode" |> WhenExpr.parse,
      ),
      bind(
        ~key="<C-\\>n",
        ~command=Commands.Oni.normalMode.id,
        ~condition="terminalFocus && insertMode" |> WhenExpr.parse,
      ),
      // Normal mode -> insert mode
      bind(
        ~key="o",
        ~command=Commands.Oni.insertMode.id,
        ~condition="terminalFocus && normalMode" |> WhenExpr.parse,
      ),
      bind(
        ~key="<S-O>",
        ~command=Commands.Oni.insertMode.id,
        ~condition="terminalFocus && normalMode" |> WhenExpr.parse,
      ),
      bind(
        ~key="Shift+a",
        ~command=Commands.Oni.insertMode.id,
        ~condition="terminalFocus && normalMode" |> WhenExpr.parse,
      ),
      bind(
        ~key="a",
        ~command=Commands.Oni.insertMode.id,
        ~condition="terminalFocus && normalMode" |> WhenExpr.parse,
      ),
      bind(
        ~key="i",
        ~command=Commands.Oni.insertMode.id,
        ~condition="terminalFocus && normalMode" |> WhenExpr.parse,
      ),
      bind(
        ~key="Shift+i",
        ~command=Commands.Oni.insertMode.id,
        ~condition="terminalFocus && normalMode" |> WhenExpr.parse,
      ),
      // Paste - Windows:
      bind(
        ~key="<C-V>",
        ~command=Feature_Clipboard.Commands.paste.id,
        ~condition="terminalFocus && insertMode && isWin" |> WhenExpr.parse,
      ),
      // Paste - Linux:
      bind(
        ~key="<C-S-V>",
        ~command=Feature_Clipboard.Commands.paste.id,
        ~condition="terminalFocus && insertMode && isLinux" |> WhenExpr.parse,
      ),
      // Paste - Mac:
      bind(
        ~key="<D-V>",
        ~command=Feature_Clipboard.Commands.paste.id,
        ~condition="terminalFocus && insertMode && isMac" |> WhenExpr.parse,
      ),
    ];
  };
};
