%option noyywrap
%{
/*****************声明和选项设置  begin*****************/
#include <stdio.h>
#include <stdlib.h>

#include "syntax_tree.h"
#include "syntax_analyzer.h"

int lines = 1;
int pos_start = 1;
int pos_end = 1;

void pass_node(char *text){
     yylval.node = new_syntax_tree_node(text);
}

/* Advance the line/column counters across `text` (used for skipped
 * whitespace, newlines and comments, which may contain embedded '\n'). */
void advance_pos(char *text){
    int i;
    for (i = 0; text[i] != '\0'; i++) {
        if (text[i] == '\n') {
            lines++;
            pos_end = 1;
        } else {
            pos_end++;
        }
    }
}

/* Record the starting column, move the end column past the token, hand the
 * token text to bison as a tree leaf, and return the token type. */
#define emit(tok) { pos_start = pos_end; pos_end += yyleng; pass_node(yytext); return tok; }

/*****************声明和选项设置  end*****************/

%}


%%
 /* ---------- skip whitespace, newlines and comments ---------- */
[ \t\r\f\v]+                    { advance_pos(yytext); }
\n                              { advance_pos(yytext); }
"/*"([^*]|\*+[^*/])*\*+"/"      { advance_pos(yytext); }

 /* ---------- keywords ---------- */
"else"                          { emit(ELSE); }
"if"                            { emit(IF); }
"int"                           { emit(INT); }
"float"                         { emit(FLOAT); }
"void"                          { emit(VOID); }
"while"                         { emit(WHILE); }
"return"                        { emit(RETURN); }

 /* ---------- identifiers (letters only, per lab spec) ---------- */
[a-zA-Z]+                       { emit(ID); }

 /* ---------- numbers ---------- */
 /* FLOATNUM: 123.  /  123.4  /  .4 ; must come before NUM and before '.',
  * because flex picks the longest match and these must not split. */
[0-9]+\.[0-9]*|\.[0-9]+         { emit(FLOATNUM); }
[0-9]+                          { emit(NUM); }

 /* ---------- operators and punctuation ---------- */
"+"                             { emit(PLUS); }
"-"                             { emit(MINUS); }
"*"                             { emit(MUL); }
"/"                             { emit(DIV); }
"<"                             { emit(LT); }
"<="                            { emit(LE); }
">"                             { emit(GT); }
">="                            { emit(GE); }
"=="                            { emit(EQ); }
"!="                            { emit(NEQ); }
"="                             { emit(ASSIGN); }
";"                             { emit(SEMI); }
","                             { emit(COMMA); }
"("                             { emit(LPAREN); }
")"                             { emit(RPAREN); }
"["                             { emit(LBRACKET); }
"]"                             { emit(RBRACKET); }
"{"                             { emit(LBRACE); }
"}"                             { emit(RBRACE); }

 /* ---------- any other single character is an error ---------- */
.                               { emit(ERROR); }

%%
