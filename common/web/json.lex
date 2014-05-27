%{
  //#define YY_DECL int jsonlex(yyscan_t yyscanner)
  #include <stdio.h>
  #include "common/web/LexerGlue.h"
%}

%option outfile="json.yy.cpp" header-file="json.yy.h"
%option yylineno
%option noyywrap nounput noinput
%option reentrant
%option stack
%option extra-type="ola::web::LexerGlue *"

DIGIT1to9 [1-9]
DIGIT [0-9]
DIGITS {DIGIT}+
INT {DIGIT}|{DIGIT1to9}{DIGITS}|-{DIGIT}|-{DIGIT1to9}{DIGITS}
FRAC [.]{DIGITS}
EXP {E}{DIGITS}
E [eE][+-]?
HEX_DIGIT [0-9a-f]
NUMBER {INT}|{INT}{FRAC}|{INT}{EXP}|{INT}{FRAC}{EXP}
UNESCAPEDCHAR [ -!#-\[\]-~]
ESCAPEDCHAR \\["\\bfnrt/]
UNICODECHAR \\u{HEX_DIGIT}{HEX_DIGIT}{HEX_DIGIT}{HEX_DIGIT}
CHAR {UNESCAPEDCHAR}|{ESCAPEDCHAR}|{UNICODECHAR}
CHARS {CHAR}+
DBL_QUOTE ["]
WHITESPACE [ \t\n]
%%
{DBL_QUOTE}{DBL_QUOTE} |
{DBL_QUOTE}{CHARS}{DBL_QUOTE} {
    yyextra->String(yytext);
};
{INT} {
  yyextra->Int(yytext);
}
{FRAC} {
  yyextra->Fractional(yytext);
}
{EXP} {
  yyextra->Exponent(yytext);
}
true {
  yyextra->Bool(true);
};
false {
  yyextra->Bool(false);
};
null {
  yyextra->Null();
};
\{ {
  yyextra->OpenObject();
};
\} {
  yyextra->CloseObject();
};
\[ {
  yyextra->OpenArray();
};
\] {
  yyextra->CloseArray();
};
, {
  yyextra->Comma();
};
: {
  yyextra->Colon();
};
{WHITESPACE}
. {
  // yyextra->SetError();
  printf("Unexpected: `%c'\nExiting...\n",*yytext);
  exit(0);
}
%%

/*
int main(int argc, char *argv[]) {
  const char input[] = "[1,  2,  3]";
  int a = 10;
  yyscan_t scanner;
  yylex_init_extra(&a, &scanner);
  YY_BUFFER_STATE buf = yy_scan_string(input, scanner);
  // parser->Begin();
  jsonlex(scanner);
  yy_delete_buffer(buf, scanner);
  printf("\n");
  // parser->End();
  yylex_destroy(scanner);
}
*/
