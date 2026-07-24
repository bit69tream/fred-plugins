typedef enum VimState {
  VIM_STATE_Command,
  VIM_STATE_Default = VIM_STATE_Command,
  VIM_STATE_Insert,
  VIM_STATE_Replace,
  VIM_STATE_Visual,
} VimState;

typedef enum VimSubCommand {
  VIM_SUBCMD_None = '\0',
  VIM_SUBCMD_c    = 'c',
  VIM_SUBCMD_g    = 'g',
  VIM_SUBCMD_y    = 'y',
  VIM_SUBCMD_d    = 'd',
  VIM_SUBCMD_r    = 'r',
  VIM_SUBCMD_Z    = 'Z',
  VIM_SUBCMD_dg   = '@',
  VIM_SUBCMD_ci   = '`',
} VimSubCommand;

typedef struct VimProcessorState {
  VimState state;
  VimSubCommand sub_cmd;
  uint32_t repeat;
} VimProcessorState;

typedef struct VimCmdBufferEntry {
  struct VimCmdBufferEntry* next;
  EditorCmd cmd;
} VimCmdBufferEntry;

typedef struct VimCmdBufferList {
  VimCmdBufferEntry* first;
  VimCmdBufferEntry* last;
  uint64_t count;
} VimCmdBufferList;

// Data structure macros (mostly borrowed from RADDBG).
#define CheckNil(nil,p) ((p) == 0 || (p) == nil)
#define SetNil(nil,p) ((p) = nil)

#define EachNode(T, it, first) (T *it = first; it != 0; it = it->next)

#define SLLQueuePush_NZ(nil,f,l,n,next) (CheckNil(nil,f)?\
((f)=(l)=(n),SetNil(nil,(n)->next)):\
((l)->next=(n),(l)=(n),SetNil(nil,(n)->next)))

#define SLLQueuePush(f,l,n) SLLQueuePush_NZ(0,f,l,n,next)

// Helpers to insert into vim command buffer.
void push_vim_cmd_entry(Arena* arena, VimCmdBufferList* lst, EditorCmd* cmd) {
  VimCmdBufferEntry* entry = push_array(arena, VimCmdBufferEntry, 1);
  entry->cmd = *cmd;
  SLLQueuePush(lst->first, lst->last, entry);
  ++lst->count;
}

void flush_vim_cmd_list(EditorCtx* ctx, VimProcessorState* vim_state, VimCmdBufferList* lst) {
  uint32_t rep_count = 1;
  // We only want to consider repeat once commands are queued up.
  if (vim_state->repeat > 1 && lst->count != 0) {
    rep_count += vim_state->repeat - 1;
    // Consume the repeat value.
    vim_state->repeat = 0;
  }

  do {
    for EachNode(VimCmdBufferEntry, n, lst->first) {
      ed_push_command(ctx, &n->cmd);
    }
    --rep_count;
  } while (rep_count > 0);
}

// Various helpers.
#define ed_flag_remove(e, f) ((e) & (~(f)))

#define vim_change_state(x, s) x->state = s
#define vim_insert_like(s) (s->state == VIM_STATE_Insert || s->state == VIM_STATE_Replace)

void vim_esc(EditorCtx* ctx, VimProcessorState* vim_state) {
  // First, clear all selections.
  EditorCmd cmd = {0};
  cmd.cmd = ED_SelectionClearSelections;
  ed_push_command(ctx, &cmd);
  // If we were in command mode already, clear all the multi-cursors.
  if (vim_state->state == VIM_STATE_Command) {
    cmd.cmd = ED_MCDropCursors;
    ed_push_command(ctx, &cmd);
  }

  // Vim also has this behavior where moving from insert mode -> default mode
  // causes the cursor to nav left (but not navigate to previous line).
  if (vim_state->state == VIM_STATE_Insert) {
    cmd.cmd = ED_NavLeftNoNLAdvance;
    ed_push_command(ctx, &cmd);
  }
  vim_change_state(vim_state, VIM_STATE_Default);
  vim_state->sub_cmd = VIM_SUBCMD_None;
  vim_state->repeat = 0;
}

void vim_process_cmd(EditorCtx* ctx, UIState* state, VimProcessorState* vim_state, char c) {
  Temp scratch = scratch_begin(NULL);
  EditorCmd cmd = {0};
  VimCmdBufferList cmd_lst = {0};
  uint32_t captured_repeat = 0;
  VimSubCommand prev_sub_cmd = vim_state->sub_cmd;
  vim_state->sub_cmd = VIM_SUBCMD_None;

  // Process subcommands which take priority.
  if (prev_sub_cmd == VIM_SUBCMD_r) {
    cmd.cmd = ED_InsReplace;
    cmd.buf = str8(&c, 1);
    push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
  }
  else {
    // Command processing.
    switch (c) {
    // Capture repeats.
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      vim_state->repeat *= 10;
      vim_state->repeat += (c - '0');
      // Preserve whatever command we were processing.
      vim_state->sub_cmd = prev_sub_cmd;
      captured_repeat = 1;
      break;
    // Movement.
    case 'h':
      cmd.cmd = ED_NavLeft;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'j':
      if (prev_sub_cmd == VIM_SUBCMD_d) {
        // Since we're moving down we want to delete this line and delete a line again.
        // Suppress the first one so the undo groups them.
        cmd.cmd = ED_DelDeleteLine;
        cmd.flags = ED_FLG_SuppressHistory;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        cmd.flags = ed_flag_remove(cmd.flags, ED_FLG_SuppressHistory);
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else {
        cmd.cmd = ED_NavLineDown;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      break;
    case 'k':
      cmd.cmd = ED_NavLineUp;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'l':
      cmd.cmd = ED_NavRight;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'H':
      {
        // Make the camera recenter.
        cmd.flags |= ED_FLG_ResetCamera;
        cmd.cmd = ED_NavCursorTopScreen;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      break;
    case 'L':
      {
        // Make the camera recenter.
        cmd.flags |= ED_FLG_ResetCamera;
        cmd.cmd = ED_NavCursorBottomScreen;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      break;
    case 'M':
      {
        // Make the camera recenter.
        cmd.flags |= ED_FLG_ResetCamera;
        cmd.cmd = ED_NavCursorCenterScreen;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      break;
    case '0':
      // Process repeats first.
      if (vim_state->repeat != 0) {
        vim_state->repeat *= 10;
        // Preserve whatever command we were processing.
        vim_state->sub_cmd = prev_sub_cmd;
        captured_repeat = 1;
      }
      else if (prev_sub_cmd == VIM_SUBCMD_d) {
        cmd.cmd = ED_NavBeginningOfLine;
        cmd.flags |= ED_FLG_UpdateSelection;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        // Now we can delete the selection.
        cmd.flags = ed_flag_remove(cmd.flags, ED_FLG_UpdateSelection);
        cmd.cmd = ED_DelDeleteChar;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else {
        cmd.cmd = ED_NavBeginningOfLine;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      break;
    case '^':
      if (prev_sub_cmd == VIM_SUBCMD_d) {
        cmd.cmd = ED_NavFirstNonemptyOfLine;
        cmd.flags |= ED_FLG_UpdateSelection;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        // Now we can delete the selection.
        cmd.flags = ed_flag_remove(cmd.flags, ED_FLG_UpdateSelection);
        cmd.cmd = ED_DelDeleteChar;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else {
        cmd.cmd = ED_NavFirstNonemptyOfLine;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      break;
    case '$':
      if (prev_sub_cmd == VIM_SUBCMD_d) {
        cmd.cmd = ED_NavEndOfLine;
        cmd.flags |= ED_FLG_UpdateSelection;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        // Now we can delete the selection.
        cmd.flags = ed_flag_remove(cmd.flags, ED_FLG_UpdateSelection);
        cmd.cmd = ED_DelDeleteChar;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else if (prev_sub_cmd == VIM_SUBCMD_c) {
        cmd.cmd = ED_NavEndOfLine;
        cmd.flags |= ED_FLG_UpdateSelection;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        // Now delete it.
        cmd.flags = ed_flag_remove(cmd.flags, ED_FLG_UpdateSelection);
        cmd.cmd = ED_DelDeleteChar;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        // Pull into insert.
        vim_change_state(vim_state, VIM_STATE_Insert);
      }
      else {
        cmd.cmd = ED_NavEndOfLine;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      break;
    case 'G':
      if (prev_sub_cmd == VIM_SUBCMD_d) {
        cmd.cmd = ED_NavContentEnd;
        cmd.flags |= ED_FLG_UpdateSelection;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        // Now we can delete the selection.
        cmd.flags = ed_flag_remove(cmd.flags, ED_FLG_UpdateSelection);
        cmd.cmd = ED_DelDeleteChar;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else {
        cmd.cmd = ED_NavContentEnd;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      break;
    case 'g':
      if (prev_sub_cmd == VIM_SUBCMD_g) {
        cmd.cmd = ED_NavContentBeginning;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else if (prev_sub_cmd == VIM_SUBCMD_dg) {
        cmd.cmd = ED_NavContentBeginning;
        cmd.flags |= ED_FLG_UpdateSelection;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        // Now we can delete the selection.
        cmd.flags = ed_flag_remove(cmd.flags, ED_FLG_UpdateSelection);
        cmd.cmd = ED_DelDeleteChar;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else if (prev_sub_cmd == VIM_SUBCMD_d) {
        vim_state->sub_cmd = VIM_SUBCMD_dg;
      }
      else {
        vim_state->sub_cmd = VIM_SUBCMD_g;
      }
      break;
    case '%':
      cmd.cmd = ED_NavMatchingEncloser;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'e': // FIXME: find correct behavior for 'e'.
    case 'w':
      if (prev_sub_cmd == VIM_SUBCMD_d) {
        cmd.cmd = ED_DelDeleteWord;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else if (prev_sub_cmd == VIM_SUBCMD_c) {
        cmd.cmd = ED_DelDeleteWord;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        vim_change_state(vim_state, VIM_STATE_Insert);
      }
      else if (prev_sub_cmd == VIM_SUBCMD_ci) {
        cmd.cmd = ED_NavWordStart;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        // Then select the end of the word.
        cmd.cmd = ED_NavWordEnd;
        cmd.flags |= ED_FLG_UpdateSelection;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        // Delete.
        cmd.flags = ed_flag_remove(cmd.flags, ED_FLG_UpdateSelection);
        cmd.cmd = ED_DelDeleteChar;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        vim_change_state(vim_state, VIM_STATE_Insert);
      }
      else {
        cmd.cmd = ED_NavWordRight;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      break;
    case 'E': // FIXME: find correct behavior for 'E'.
    case 'W':
      if (prev_sub_cmd == VIM_SUBCMD_d) {
        cmd.cmd = ED_DelDeleteChunk;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else {
        cmd.cmd = ED_NavChunkRight;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      break;
    case 'b':
      if (prev_sub_cmd == VIM_SUBCMD_d) {
        cmd.cmd = ED_DelBackspaceWord;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else {
        cmd.cmd = ED_NavWordLeft;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      break;
    case 'B':
      if (prev_sub_cmd == VIM_SUBCMD_d) {
        cmd.cmd = ED_DelBackspaceChunk;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else {
        cmd.cmd = ED_NavChunkLeft;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      break;
    case '}':
      cmd.cmd = ED_NavEmptyBlockDown;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case '{':
      cmd.cmd = ED_NavEmptyBlockUp;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case ':':
      cmd.cmd = ED_NavRequestGotoLine;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    // Insertion-like behavior.
    case 'i':
      if (prev_sub_cmd == VIM_SUBCMD_c) {
        vim_state->sub_cmd = VIM_SUBCMD_ci;
      }
      else {
        vim_change_state(vim_state, VIM_STATE_Insert);
      }
      break;
    case 'I':
      vim_change_state(vim_state, VIM_STATE_Insert);
      cmd.cmd = ED_NavFirstNonemptyOfLine;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'o':
      vim_change_state(vim_state, VIM_STATE_Insert);
      cmd.cmd = ED_InsOpenLineBelow;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'O':
      vim_change_state(vim_state, VIM_STATE_Insert);
      cmd.cmd = ED_InsOpenLineAbove;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'a':
      vim_change_state(vim_state, VIM_STATE_Insert);
      cmd.cmd = ED_NavRightNoNLAdvance;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'A':
      vim_change_state(vim_state, VIM_STATE_Insert);
      cmd.cmd = ED_NavEndOfLine;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'R':
      vim_change_state(vim_state, VIM_STATE_Replace);
      break;
    // Editing shortcuts.
    case 'u': // Redo will share the original shortcut.
      cmd.cmd = ED_URTryUndo;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'd':
      if (prev_sub_cmd == VIM_SUBCMD_d) {
        cmd.cmd = ED_RequestClipboardCopy;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        cmd.cmd = ED_DelDeleteLine;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else {
        vim_state->sub_cmd = VIM_SUBCMD_d;
      }
      break;
    case 'x':
      // Highlight the char in front.
      cmd.flags |= ED_FLG_UpdateSelection;
      cmd.cmd = ED_NavRight;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      // Cut it out.
      cmd.flags = ed_flag_remove(cmd.flags, ED_FLG_UpdateSelection);
      cmd.cmd = ED_RequestClipboardCut;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'D':
      cmd.cmd = ED_NavEndOfLine;
      cmd.flags |= ED_FLG_UpdateSelection;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      // Now delete it.
      cmd.flags = ed_flag_remove(cmd.flags, ED_FLG_UpdateSelection);
      cmd.cmd = ED_DelDeleteChar;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'C':
      cmd.cmd = ED_NavEndOfLine;
      cmd.flags |= ED_FLG_UpdateSelection;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      // Now delete it.
      cmd.flags = ed_flag_remove(cmd.flags, ED_FLG_UpdateSelection);
      cmd.cmd = ED_DelDeleteChar;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      // Pull into insert.
      vim_change_state(vim_state, VIM_STATE_Insert);
      break;
    case 'c':
      if (prev_sub_cmd == VIM_SUBCMD_c) {
        cmd.cmd = ED_NavBeginningOfLine;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        // Select entire line content.
        cmd.cmd = ED_NavEndOfLine;
        cmd.flags |= ED_FLG_UpdateSelection;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        // Delete it.
        cmd.flags = ed_flag_remove(cmd.flags, ED_FLG_UpdateSelection);
        cmd.cmd = ED_DelDeleteChar;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        // Pull back to insert mode.
        vim_change_state(vim_state, VIM_STATE_Insert);
      }
      else {
        vim_state->sub_cmd = VIM_SUBCMD_c;
      }
      break;
    case 'J':
      // TODO: multi-cursor.
      cmd.cmd = ED_SpecJoinLineBelow;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'r':
      vim_state->sub_cmd = VIM_SUBCMD_r;
      break;
    // Search.
    case '/':
      cmd.cmd = ED_FindRequestFind;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case '*':
      cmd.cmd = ED_FindRequestFindWithSeed;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    // Yank/Paste.
    case 'y':
      if (prev_sub_cmd == VIM_SUBCMD_y) {
        cmd.cmd = ED_RequestClipboardCopy;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else {
        vim_state->sub_cmd = VIM_SUBCMD_y;
      }
      break;
    case 'p':
      cmd.cmd = ED_RequestClipboardPaste;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    case 'P':
      cmd.cmd = ED_RequestClipboardPasteBeforeCursor;
      push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      break;
    // Visual mode engage!
    case 'v':
      vim_change_state(vim_state, VIM_STATE_Visual);
      break;
    // Closing.
    case 'Z':
      if (prev_sub_cmd == VIM_SUBCMD_Z) {
        cmd.cmd = ED_SaveRequestSave;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
        cmd.cmd = ED_CloseRequestClose;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      else {
        vim_state->sub_cmd = VIM_SUBCMD_Z;
      }
      break;
    case 'Q':
      if (prev_sub_cmd == VIM_SUBCMD_Z) {
        cmd.cmd = ED_CloseRequestClose;
        push_vim_cmd_entry(scratch.arena, &cmd_lst, &cmd);
      }
      break;
    }
  }

  flush_vim_cmd_list(ctx, vim_state, &cmd_lst);
  // Keep repeats active if we're processing a subcommand or we processed a repeat.
  vim_state->repeat = vim_state->repeat *
      (captured_repeat || vim_state->sub_cmd != VIM_SUBCMD_None);

  scratch_end(scratch);
}

void vim_process(EditorCtx* ctx, UIState* state, VimProcessorState* vim_state, char c) {
  EditorCmd cmd = {0};
  uint32_t processed = 0;
  if (mod_active(OS_KEY_MOD_Ctrl)) {
    if (vim_state->state == VIM_STATE_Visual) {
        switch (c) {
        case 'd':
          cmd.cmd = ED_NavPageDown;
          cmd.flags |= ED_FLG_UpdateSelection;
          ed_push_command(ctx, &cmd);
          break;
        case 'u':
          cmd.cmd = ED_NavPageUp;
          cmd.flags |= ED_FLG_UpdateSelection;
          ed_push_command(ctx, &cmd);
          break;
        }
    }
    else {
      // Basic navigation.
      switch (c)
      {
      case 'd':
        cmd.cmd = ED_NavPageDown;
        ed_push_command(ctx, &cmd);
        break;
      case 'u':
        cmd.cmd = ED_NavPageUp;
        ed_push_command(ctx, &cmd);
        break;
      case 'j':
        if (mod_active(OS_KEY_MOD_Alt)) {
          cmd.cmd = ED_MCDupCursorDown;
          ed_push_command(ctx, &cmd);
        }
        break;
      case 'k':
        if (mod_active(OS_KEY_MOD_Alt)) {
          cmd.cmd = ED_MCDupCursorUp;
          ed_push_command(ctx, &cmd);
        }
        break;
      }
    }
    processed = 1;
  }

  if (!processed) {
    switch (vim_state->state) {
    case VIM_STATE_Insert:
      cmd.cmd = ED_InsInsert;
      cmd.buf = str8(&c, 1);
      ed_push_command(ctx, &cmd);
      break;
    case VIM_STATE_Replace:
      cmd.cmd = ED_InsOverwriteInsertBuf;
      cmd.buf = str8(&c, 1);
      ed_push_command(ctx, &cmd);
      break;
    case VIM_STATE_Visual:
      //vim_process_vis(data, c);
      break;
    default:
      vim_process_cmd(ctx, state, vim_state, c);
      break;
    }
  }
}