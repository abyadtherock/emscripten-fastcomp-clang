#include "clang/AST/CommentLexer.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ErrorHandling.h"

namespace clang {
namespace comments {

void Token::dump(const Lexer &L, const SourceManager &SM) const {
  llvm::errs() << "comments::Token Kind=" << Kind << " ";
  Loc.dump(SM);
  llvm::errs() << " " << Length << " \"" << L.getSpelling(*this, SM) << "\"\n";
}

bool Lexer::isVerbatimBlockCommand(StringRef BeginName,
                                  StringRef &EndName) const {
  const char *Result = llvm::StringSwitch<const char *>(BeginName)
    .Case("code", "endcode")
    .Case("verbatim", "endverbatim")
    .Case("htmlonly", "endhtmlonly")
    .Case("latexonly", "endlatexonly")
    .Case("xmlonly", "endxmlonly")
    .Case("manonly", "endmanonly")
    .Case("rtfonly", "endrtfonly")

    .Case("dot", "enddot")
    .Case("msc", "endmsc")

    .Case("f$", "f$") // Inline LaTeX formula
    .Case("f[", "f]") // Displayed LaTeX formula
    .Case("f{", "f}") // LaTeX environment

    .Default(NULL);

  if (Result) {
    EndName = Result;
    return true;
  }

  for (VerbatimBlockCommandVector::const_iterator
           I = VerbatimBlockCommands.begin(),
           E = VerbatimBlockCommands.end();
       I != E; ++I)
    if (I->BeginName == BeginName) {
      EndName = I->EndName;
      return true;
    }

  return false;
}

bool Lexer::isVerbatimLineCommand(StringRef Name) const {
  bool Result = llvm::StringSwitch<bool>(Name)
  .Case("fn", true)
  .Case("var", true)
  .Case("property", true)
  .Case("typedef", true)

  .Case("overload", true)

  .Case("defgroup", true)
  .Case("ingroup", true)
  .Case("addtogroup", true)
  .Case("weakgroup", true)
  .Case("name", true)

  .Case("section", true)
  .Case("subsection", true)
  .Case("subsubsection", true)
  .Case("paragraph", true)

  .Case("mainpage", true)
  .Case("subpage", true)
  .Case("ref", true)

  .Default(false);

  if (Result)
    return true;

  for (VerbatimLineCommandVector::const_iterator
           I = VerbatimLineCommands.begin(),
           E = VerbatimLineCommands.end();
       I != E; ++I)
    if (I->Name == Name)
      return true;

  return false;
}

void Lexer::skipLineStartingDecorations() {
  // This function should be called only for C comments
  assert(CommentState == LCS_InsideCComment);

  if (BufferPtr == CommentEnd)
    return;

  switch (*BufferPtr) {
  case ' ':
  case '\t':
  case '\f':
  case '\v': {
    const char *NewBufferPtr = BufferPtr;
    NewBufferPtr++;
    if (NewBufferPtr == CommentEnd)
      return;

    char C = *NewBufferPtr;
    while (C == ' ' || C == '\t' || C == '\f' || C == '\v') {
      NewBufferPtr++;
      if (NewBufferPtr == CommentEnd)
        return;
      C = *NewBufferPtr;
    }
    if (C == '*')
      BufferPtr = NewBufferPtr + 1;
    break;
  }
  case '*':
    BufferPtr++;
    break;
  }
}

namespace {
const char *findNewline(const char *BufferPtr, const char *BufferEnd) {
  for ( ; BufferPtr != BufferEnd; ++BufferPtr) {
    const char C = *BufferPtr;
    if (C == '\n' || C == '\r')
      return BufferPtr;
  }
  return BufferEnd;
}

const char *skipNewline(const char *BufferPtr, const char *BufferEnd) {
  if (BufferPtr == BufferEnd)
    return BufferPtr;

  if (*BufferPtr == '\n')
    BufferPtr++;
  else {
    assert(*BufferPtr == '\r');
    BufferPtr++;
    if (BufferPtr != BufferEnd && *BufferPtr == '\n')
      BufferPtr++;
  }
  return BufferPtr;
}

bool isHTMLIdentifierCharacter(char C) {
  return (C >= 'a' && C <= 'z') ||
         (C >= 'A' && C <= 'Z') ||
         (C >= '0' && C <= '9');
}

const char *skipHTMLIdentifier(const char *BufferPtr, const char *BufferEnd) {
  for ( ; BufferPtr != BufferEnd; ++BufferPtr) {
    if (!isHTMLIdentifierCharacter(*BufferPtr))
      return BufferPtr;
  }
  return BufferEnd;
}

/// Skip HTML string quoted in single or double quotes.  Escaping quotes inside
/// string allowed.
///
/// Returns pointer to closing quote.
const char *skipHTMLQuotedString(const char *BufferPtr, const char *BufferEnd)
{
  const char Quote = *BufferPtr;
  assert(Quote == '\"' || Quote == '\'');

  BufferPtr++;
  for ( ; BufferPtr != BufferEnd; ++BufferPtr) {
    const char C = *BufferPtr;
    if (C == Quote && BufferPtr[-1] != '\\')
      return BufferPtr;
  }
  return BufferEnd;
}

bool isHorizontalWhitespace(char C) {
  return C == ' ' || C == '\t' || C == '\f' || C == '\v';
}

bool isWhitespace(char C) {
  return C == ' ' || C == '\n' || C == '\r' ||
         C == '\t' || C == '\f' || C == '\v';
}

const char *skipWhitespace(const char *BufferPtr, const char *BufferEnd) {
  for ( ; BufferPtr != BufferEnd; ++BufferPtr) {
    if (!isWhitespace(*BufferPtr))
      return BufferPtr;
  }
  return BufferEnd;
}

bool isCommandNameCharacter(char C) {
  return (C >= 'a' && C <= 'z') ||
         (C >= 'A' && C <= 'Z') ||
         (C >= '0' && C <= '9');
}

const char *skipCommandName(const char *BufferPtr, const char *BufferEnd) {
  for ( ; BufferPtr != BufferEnd; ++BufferPtr) {
    if (!isCommandNameCharacter(*BufferPtr))
      return BufferPtr;
  }
  return BufferEnd;
}

/// Return the one past end pointer for BCPL comments.
/// Handles newlines escaped with backslash or trigraph for backslahs.
const char *findBCPLCommentEnd(const char *BufferPtr, const char *BufferEnd) {
  const char *CurPtr = BufferPtr;
  while (CurPtr != BufferEnd) {
    char C = *CurPtr;
    while (C != '\n' && C != '\r') {
      CurPtr++;
      if (CurPtr == BufferEnd)
        return BufferEnd;
      C = *CurPtr;
    }
    // We found a newline, check if it is escaped.
    const char *EscapePtr = CurPtr - 1;
    while(isHorizontalWhitespace(*EscapePtr))
      EscapePtr--;

    if (*EscapePtr == '\\' ||
        (EscapePtr - 2 >= BufferPtr && EscapePtr[0] == '/' &&
         EscapePtr[-1] == '?' && EscapePtr[-2] == '?')) {
      // We found an escaped newline.
      CurPtr = skipNewline(CurPtr, BufferEnd);
    } else
      return CurPtr; // Not an escaped newline.
  }
  return BufferEnd;
}

/// Return the one past end pointer for C comments.
/// Very dumb, does not handle escaped newlines or trigraphs.
const char *findCCommentEnd(const char *BufferPtr, const char *BufferEnd) {
  for ( ; BufferPtr != BufferEnd; ++BufferPtr) {
    if (*BufferPtr == '*') {
      assert(BufferPtr + 1 != BufferEnd);
      if (*(BufferPtr + 1) == '/')
        return BufferPtr;
    }
  }
  llvm_unreachable("buffer end hit before '*/' was seen");
}
} // unnamed namespace

void Lexer::lexCommentText(Token &T) {
  assert(CommentState == LCS_InsideBCPLComment ||
         CommentState == LCS_InsideCComment);

  switch (State) {
  case LS_Normal:
    break;
  case LS_VerbatimBlockFirstLine:
    lexVerbatimBlockFirstLine(T);
    return;
  case LS_VerbatimBlockBody:
    lexVerbatimBlockBody(T);
    return;
  case LS_HTMLOpenTag:
    lexHTMLOpenTag(T);
    return;
  }

  assert(State == LS_Normal);

  const char *TokenPtr = BufferPtr;
  assert(TokenPtr < CommentEnd);
  while (TokenPtr != CommentEnd) {
    switch(*TokenPtr) {
      case '\\':
      case '@': {
        TokenPtr++;
        if (TokenPtr == CommentEnd) {
          formTokenWithChars(T, TokenPtr, tok::text);
          T.setText(StringRef(BufferPtr - T.getLength(), T.getLength()));
          return;
        }
        char C = *TokenPtr;
        switch (C) {
        default:
          break;

        case '\\': case '@': case '&': case '$':
        case '#':  case '<': case '>': case '%':
        case '\"': case '.': case ':':
          // This is one of \\ \@ \& \$ etc escape sequences.
          TokenPtr++;
          if (C == ':' && TokenPtr != CommentEnd && *TokenPtr == ':') {
            // This is the \:: escape sequence.
            TokenPtr++;
          }
          formTokenWithChars(T, TokenPtr, tok::text);
          T.setText(StringRef(BufferPtr - (T.getLength() - 1),
                              T.getLength() - 1));
          return;
        }

        // Don't make zero-length commands.
        if (!isCommandNameCharacter(*TokenPtr)) {
          formTokenWithChars(T, TokenPtr, tok::text);
          T.setText(StringRef(BufferPtr - T.getLength(), T.getLength()));
          return;
        }

        TokenPtr = skipCommandName(TokenPtr, CommentEnd);
        unsigned Length = TokenPtr - (BufferPtr + 1);

        // Hardcoded support for lexing LaTeX formula commands
        // \f$ \f[ \f] \f{ \f} as a single command.
        if (Length == 1 && TokenPtr[-1] == 'f' && TokenPtr != CommentEnd) {
          C = *TokenPtr;
          if (C == '$' || C == '[' || C == ']' || C == '{' || C == '}') {
            TokenPtr++;
            Length++;
          }
        }

        const StringRef CommandName(BufferPtr + 1, Length);
        StringRef EndName;

        if (isVerbatimBlockCommand(CommandName, EndName)) {
          setupAndLexVerbatimBlock(T, TokenPtr, *BufferPtr, EndName);
          return;
        }
        if (isVerbatimLineCommand(CommandName)) {
          lexVerbatimLine(T, TokenPtr);
          return;
        }
        formTokenWithChars(T, TokenPtr, tok::command);
        T.setCommandName(CommandName);
        return;
      }

      case '<': {
        TokenPtr++;
        if (TokenPtr == CommentEnd) {
          formTokenWithChars(T, TokenPtr, tok::text);
          T.setText(StringRef(BufferPtr - T.getLength(), T.getLength()));
          return;
        }
        const char C = *TokenPtr;
        if (isHTMLIdentifierCharacter(C))
          setupAndLexHTMLOpenTag(T);
        else if (C == '/')
          lexHTMLCloseTag(T);
        return;
      }

      case '\n':
      case '\r':
        TokenPtr = skipNewline(TokenPtr, CommentEnd);
        formTokenWithChars(T, TokenPtr, tok::newline);

        if (CommentState == LCS_InsideCComment)
          skipLineStartingDecorations();
        return;

      default: {
        while (true) {
          TokenPtr++;
          if (TokenPtr == CommentEnd)
            break;
          char C = *TokenPtr;
          if(C == '\n' || C == '\r' ||
             C == '\\' || C == '@' || C == '<')
            break;
        }
        formTokenWithChars(T, TokenPtr, tok::text);
        T.setText(StringRef(BufferPtr - T.getLength(), T.getLength()));
        return;
      }
    }
  }
}

void Lexer::setupAndLexVerbatimBlock(Token &T,
                                     const char *TextBegin,
                                     char Marker, StringRef EndName) {
  VerbatimBlockEndCommandName.clear();
  VerbatimBlockEndCommandName.append(Marker == '\\' ? "\\" : "@");
  VerbatimBlockEndCommandName.append(EndName);

  formTokenWithChars(T, TextBegin, tok::verbatim_block_begin);
  T.setVerbatimBlockName(StringRef(TextBegin - (T.getLength() - 1),
                                   T.getLength() - 1));

  State = LS_VerbatimBlockFirstLine;
}

void Lexer::lexVerbatimBlockFirstLine(Token &T) {
  assert(BufferPtr < CommentEnd);

  // FIXME: It would be better to scan the text once, finding either the block
  // end command or newline.
  //
  // Extract current line.
  const char *Newline = findNewline(BufferPtr, CommentEnd);
  StringRef Line(BufferPtr, Newline - BufferPtr);

  // Look for end command in current line.
  size_t Pos = Line.find(VerbatimBlockEndCommandName);
  const char *NextLine;
  if (Pos == StringRef::npos) {
    // Current line is completely verbatim.
    NextLine = skipNewline(Newline, CommentEnd);
  } else if (Pos == 0) {
    // Current line contains just an end command.
    const char *End = BufferPtr + VerbatimBlockEndCommandName.size();
    formTokenWithChars(T, End, tok::verbatim_block_end);
    T.setVerbatimBlockName(StringRef(End - (T.getLength() - 1),
                                     T.getLength() - 1));
    State = LS_Normal;
    return;
  } else {
    // There is some text, followed by end command.  Extract text first.
    NextLine = BufferPtr + Pos;
  }

  formTokenWithChars(T, NextLine, tok::verbatim_block_line);
  T.setVerbatimBlockText(StringRef(NextLine - T.getLength(), T.getLength()));

  State = LS_VerbatimBlockBody;
}

void Lexer::lexVerbatimBlockBody(Token &T) {
  assert(State == LS_VerbatimBlockBody);

  if (CommentState == LCS_InsideCComment)
    skipLineStartingDecorations();

  lexVerbatimBlockFirstLine(T);
}

void Lexer::lexVerbatimLine(Token &T, const char *TextBegin) {
  // Extract current line.
  const char *Newline = findNewline(BufferPtr, CommentEnd);

  const StringRef Name(BufferPtr + 1, TextBegin - BufferPtr - 1);
  const StringRef Text(TextBegin, Newline - TextBegin);

  formTokenWithChars(T, Newline, tok::verbatim_line);
  T.setVerbatimLineName(Name);
  T.setVerbatimLineText(Text);
}

void Lexer::setupAndLexHTMLOpenTag(Token &T) {
  assert(BufferPtr[0] == '<' && isHTMLIdentifierCharacter(BufferPtr[1]));
  const char *TagNameEnd = skipHTMLIdentifier(BufferPtr + 2, CommentEnd);

  formTokenWithChars(T, TagNameEnd, tok::html_tag_open);
  T.setHTMLTagOpenName(StringRef(TagNameEnd - (T.getLength() - 1),
                                 T.getLength() - 1));

  BufferPtr = skipWhitespace(BufferPtr, CommentEnd);

  if (BufferPtr != CommentEnd && *BufferPtr == '>') {
    BufferPtr++;
    return;
  }

  if (BufferPtr != CommentEnd && isHTMLIdentifierCharacter(*BufferPtr))
    State = LS_HTMLOpenTag;
}

void Lexer::lexHTMLOpenTag(Token &T) {
  assert(State == LS_HTMLOpenTag);

  const char *TokenPtr = BufferPtr;
  char C = *TokenPtr;
  if (isHTMLIdentifierCharacter(C)) {
    TokenPtr = skipHTMLIdentifier(TokenPtr, CommentEnd);
    formTokenWithChars(T, TokenPtr, tok::html_ident);
    T.setHTMLIdent(StringRef(TokenPtr - T.getLength(), T.getLength()));
  } else {
    switch (C) {
    case '=':
      TokenPtr++;
      formTokenWithChars(T, TokenPtr, tok::html_equals);
      break;
    case '\"':
    case '\'': {
      const char *OpenQuote = TokenPtr;
      TokenPtr = skipHTMLQuotedString(TokenPtr, CommentEnd);
      const char *ClosingQuote = TokenPtr;
      if (TokenPtr != CommentEnd) // Skip closing quote.
        TokenPtr++;
      formTokenWithChars(T, TokenPtr, tok::html_quoted_string);
      T.setHTMLQuotedString(StringRef(OpenQuote + 1,
                                      ClosingQuote - (OpenQuote + 1)));
      break;
    }
    case '>':
      TokenPtr++;
      formTokenWithChars(T, TokenPtr, tok::html_greater);
      break;
    }
  }

  // Now look ahead and return to normal state if we don't see any HTML tokens
  // ahead.
  BufferPtr = skipWhitespace(BufferPtr, CommentEnd);
  if (BufferPtr == CommentEnd) {
    State = LS_Normal;
    return;
  }

  C = *BufferPtr;
  if (!isHTMLIdentifierCharacter(C) &&
      C != '=' && C != '\"' && C != '\'' && C != '>') {
    State = LS_Normal;
    return;
  }
}

void Lexer::lexHTMLCloseTag(Token &T) {
  assert(BufferPtr[0] == '<' && BufferPtr[1] == '/');

  const char *TagNameBegin = skipWhitespace(BufferPtr + 2, CommentEnd);
  const char *TagNameEnd = skipHTMLIdentifier(TagNameBegin, CommentEnd);

  const char *End = skipWhitespace(TagNameEnd, CommentEnd);
  if (End != CommentEnd && *End == '>')
    End++;

  formTokenWithChars(T, End, tok::html_tag_close);
  T.setHTMLTagCloseName(StringRef(TagNameBegin, TagNameEnd - TagNameBegin));
}

Lexer::Lexer(SourceLocation FileLoc, const CommentOptions &CommOpts,
             const char *BufferStart, const char *BufferEnd):
    BufferStart(BufferStart), BufferEnd(BufferEnd),
    FileLoc(FileLoc), CommOpts(CommOpts), BufferPtr(BufferStart),
    CommentState(LCS_BeforeComment), State(LS_Normal) {
}

void Lexer::lex(Token &T) {
again:
  switch (CommentState) {
  case LCS_BeforeComment:
    if (BufferPtr == BufferEnd) {
      formTokenWithChars(T, BufferPtr, tok::eof);
      return;
    }

    assert(*BufferPtr == '/');
    BufferPtr++; // Skip first slash.
    switch(*BufferPtr) {
    case '/': { // BCPL comment.
      BufferPtr++; // Skip second slash.

      if (BufferPtr != BufferEnd) {
        // Skip Doxygen magic marker, if it is present.
        // It might be missing because of a typo //< or /*<, or because we
        // merged this non-Doxygen comment into a bunch of Doxygen comments
        // around it: /** ... */ /* ... */ /** ... */
        const char C = *BufferPtr;
        if (C == '/' || C == '!')
          BufferPtr++;
      }

      // Skip less-than symbol that marks trailing comments.
      // Skip it even if the comment is not a Doxygen one, because //< and /*<
      // are frequent typos.
      if (BufferPtr != BufferEnd && *BufferPtr == '<')
        BufferPtr++;

      CommentState = LCS_InsideBCPLComment;
      State = LS_Normal;
      CommentEnd = findBCPLCommentEnd(BufferPtr, BufferEnd);
      goto again;
    }
    case '*': { // C comment.
      BufferPtr++; // Skip star.

      // Skip Doxygen magic marker.
      const char C = *BufferPtr;
      if ((C == '*' && *(BufferPtr + 1) != '/') || C == '!')
        BufferPtr++;

      // Skip less-than symbol that marks trailing comments.
      if (BufferPtr != BufferEnd && *BufferPtr == '<')
        BufferPtr++;

      CommentState = LCS_InsideCComment;
      State = LS_Normal;
      CommentEnd = findCCommentEnd(BufferPtr, BufferEnd);
      goto again;
    }
    default:
      llvm_unreachable("second character of comment should be '/' or '*'");
    }

  case LCS_BetweenComments: {
    // Consecutive comments are extracted only if there is only whitespace
    // between them.  So we can search for the start of the next comment.
    const char *EndWhitespace = BufferPtr;
    while(EndWhitespace != BufferEnd && *EndWhitespace != '/')
      EndWhitespace++;

    // Turn any whitespace between comments (and there is only whitespace
    // between them) into a newline.  We have two newlines between comments
    // in total (first one was synthesized after a comment).
    formTokenWithChars(T, EndWhitespace, tok::newline);

    CommentState = LCS_BeforeComment;
    break;
  }

  case LCS_InsideBCPLComment:
  case LCS_InsideCComment:
    if (BufferPtr != CommentEnd) {
      lexCommentText(T);
      break;
    } else {
      // Skip C comment closing sequence.
      if (CommentState == LCS_InsideCComment) {
        assert(BufferPtr[0] == '*' && BufferPtr[1] == '/');
        BufferPtr += 2;
        assert(BufferPtr <= BufferEnd);

        // Synthenize newline just after the C comment, regardless if there is
        // actually a newline.
        formTokenWithChars(T, BufferPtr, tok::newline);

        CommentState = LCS_BetweenComments;
        break;
      } else {
        // Don't synthesized a newline after BCPL comment.
        CommentState = LCS_BetweenComments;
        goto again;
      }
    }
  }
}

StringRef Lexer::getSpelling(const Token &Tok,
                             const SourceManager &SourceMgr,
                             bool *Invalid) const {
  SourceLocation Loc = Tok.getLocation();
  std::pair<FileID, unsigned> LocInfo = SourceMgr.getDecomposedLoc(Loc);

  bool InvalidTemp = false;
  StringRef File = SourceMgr.getBufferData(LocInfo.first, &InvalidTemp);
  if (InvalidTemp) {
    *Invalid = true;
    return StringRef();
  }

  const char *Begin = File.data() + LocInfo.second;
  return StringRef(Begin, Tok.getLength());
}

void Lexer::addVerbatimBlockCommand(StringRef BeginName, StringRef EndName) {
  VerbatimBlockCommand VBC;
  VBC.BeginName = BeginName;
  VBC.EndName = EndName;
  VerbatimBlockCommands.push_back(VBC);
}

void Lexer::addVerbatimLineCommand(StringRef Name) {
  VerbatimLineCommand VLC;
  VLC.Name = Name;
  VerbatimLineCommands.push_back(VLC);
}

} // end namespace comments
} // end namespace clang
