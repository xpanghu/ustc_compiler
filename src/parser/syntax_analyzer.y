%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "syntax_tree.h"

// external functions from lex
extern int yylex();
extern int yyparse();
extern int yyrestart();
extern FILE * yyin;

// external variables from lexical_analyzer module
extern int lines;
extern char * yytext;
extern int pos_end;
extern int pos_start;

// Global syntax tree
syntax_tree *gt;

// Error reporting
void yyerror(const char *s);

// Helper functions written for you with love
syntax_tree_node *node(const char *node_name, int children_num, ...);
%}

/* Make the generated header (syntax_analyzer.h) self-contained: lexer.c and
   the flex scanner include it without including syntax_tree.h themselves. */
%code requires {
    #include "syntax_tree.h"
}

/* Every token carries a syntax-tree leaf created by the lexer's pass_node().
   The union must expose that 'node' member, otherwise yylval.node is undefined. */
%union {
    syntax_tree_node *node;
}

/* C-Minus tokens.  Each one hands a tree leaf (its literal text) to bison. */
%token <node> ERROR
%token <node> ID NUM FLOATNUM
%token <node> ELSE IF INT FLOAT VOID WHILE RETURN
%token <node> PLUS MINUS MUL DIV
%token <node> LT LE GT GE EQ NEQ ASSIGN
%token <node> SEMI COMMA LPAREN RPAREN LBRACKET RBRACKET LBRACE RBRACE

/* Nonterminals whose semantic value is a tree node. */
%type <node> program declaration-list declaration var-declaration type-specifier
%type <node> fun-declaration params param-list param compound-stmt
%type <node> local-declarations statement-list statement expression-stmt
/* dangling-else 消歧：if 被拆为 balanced-selection（else 与最近的 IF 配平）
   和 dangling-selection（else 可能悬空），保证 ELSE 只能匹配最近的 IF。 */
%type <node> balanced-selection dangling-selection balanced-body dangling-body
%type <node> closed-iteration closed-body
%type <node> iteration-stmt return-stmt expression var
%type <node> simple-expression relop additive-expression addop term mulop factor
%type <node> integer float call args arg-list

%start program

%%

/* ---------------- Top level ---------------- */

program
    : declaration-list
    { 
        $$ = node("program", 1, $1);
        gt->root = $$; 
    }
    ;

declaration-list
    : declaration-list declaration
    { 
        $$ = node("declaration-list", 2, $1, $2);
    }
    | declaration
    { 
        $$ = node("declaration-list", 1, $1);
    }
    ;

declaration
    : var-declaration
    { $$ = node("declaration", 1, $1); }
    | fun-declaration
    { $$ = node("declaration", 1, $1); }
    ;

/* ---------------- Declarations ---------------- */

var-declaration
    : type-specifier ID SEMI
    { $$ = node("var-declaration", 3, $1, $2, $3); }
    | type-specifier ID LBRACKET NUM RBRACKET SEMI
    { $$ = node("var-declaration", 6, $1, $2, $3, $4, $5, $6); }
    ;

type-specifier
    : INT
    { $$ = node("type-specifier", 1, $1); }
    | VOID
    { $$ = node("type-specifier", 1, $1); }
    | FLOAT
    { $$ = node("type-specifier", 1, $1); }
    ;

fun-declaration
    : type-specifier ID LPAREN params RPAREN compound-stmt
    { $$ = node("fun-declaration", 6, $1, $2, $3, $4, $5, $6); }
    ;

/* ---------------- Function parameters ---------------- */

params
    : param-list
    { $$ = node("params", 1, $1); }
    | VOID            /* "void" means no parameters */
    { $$ = node("params", 1, $1); }
    ;

param-list
    : param-list COMMA param
    { $$ = node("param-list", 3, $1, $2, $3); }
    | param
    { $$ = node("param-list", 1, $1); }
    ;

param
    : type-specifier ID
    { $$ = node("param", 2, $1, $2); }
    | type-specifier ID LBRACKET RBRACKET    /* array parameter */
    { $$ = node("param", 4, $1, $2, $3, $4); }
    ;

/* ---------------- Statements ---------------- */

compound-stmt
    : LBRACE local-declarations statement-list RBRACE
    { $$ = node("compound-stmt", 4, $1, $2, $3, $4); }
    ;

local-declarations
    : local-declarations var-declaration
    { $$ = node("local-declarations", 2, $1, $2); }
    | /* empty */
    { $$ = node("local-declarations", 0); }
    ;

statement-list
    : statement-list statement
    { $$ = node("statement-list", 2, $1, $2); }
    | /* empty */
    { $$ = node("statement-list", 0); }
    ;

statement
    : expression-stmt
    { $$ = node("statement", 1, $1); }
    | compound-stmt
    { $$ = node("statement", 1, $1); }
    | balanced-selection
    { $$ = node("statement", 1, $1); }
    | dangling-selection
    { $$ = node("statement", 1, $1); }
    | iteration-stmt
    { $$ = node("statement", 1, $1); }
    | return-stmt
    { $$ = node("statement", 1, $1); }
    ;

expression-stmt
    : expression SEMI
    { $$ = node("expression-stmt", 2, $1, $2); }
    | SEMI            /* empty statement */
    { $$ = node("expression-stmt", 1, $1); }
    ;

/* 有 ELSE 且配平的 if：两个分支都只能是"不会悬空 else 的语句"。 */
balanced-selection
    : IF LPAREN expression RPAREN balanced-body ELSE balanced-body
    { $$ = node("selection-stmt", 7, $1, $2, $3, $4, $5, $6, $7); }
    ;

balanced-body
    : expression-stmt
    { $$ = node("statement", 1, $1); }
    | compound-stmt
    { $$ = node("statement", 1, $1); }
    | balanced-selection
    { $$ = node("statement", 1, $1); }
    | closed-iteration
    { $$ = node("statement", 1, $1); }
    | return-stmt
    { $$ = node("statement", 1, $1); }
    ;

/* 循环体本身也可能吸收后面的 ELSE，因此在“不能吞掉外部 ELSE”的上下文中，
   只允许循环体必然配平的迭代语句。compound-stmt 由大括号定界，无此问题。 */
closed-iteration
    : WHILE LPAREN expression RPAREN closed-body
    { $$ = node("iteration-stmt", 5, $1, $2, $3, $4, $5); }
    ;

closed-body
    : expression-stmt
    { $$ = node("statement", 1, $1); }
    | compound-stmt
    { $$ = node("statement", 1, $1); }
    | balanced-selection
    { $$ = node("statement", 1, $1); }
    | closed-iteration
    { $$ = node("statement", 1, $1); }
    | return-stmt
    { $$ = node("statement", 1, $1); }
    ;

/* 可能悬空的 if：要么整个 if 没有 ELSE，要么其 ELSE 分支自身也可悬空。
   THEN 分支是任意 statement（含 dangling），因此嵌套 if 优先吸收后面的 ELSE。 */
dangling-selection
    : IF LPAREN expression RPAREN statement
    { $$ = node("selection-stmt", 5, $1, $2, $3, $4, $5); }
    | IF LPAREN expression RPAREN balanced-body ELSE dangling-body
    { $$ = node("selection-stmt", 7, $1, $2, $3, $4, $5, $6, $7); }
    ;

dangling-body
    : dangling-selection
    { $$ = node("statement", 1, $1); }
    ;

iteration-stmt
    : WHILE LPAREN expression RPAREN statement
    { $$ = node("iteration-stmt", 5, $1, $2, $3, $4, $5); }
    ;

return-stmt
    : RETURN SEMI
    { $$ = node("return-stmt", 2, $1, $2); }
    | RETURN expression SEMI
    { $$ = node("return-stmt", 3, $1, $2, $3); }
    ;

/* ---------------- Expressions ---------------- */

expression
    : var ASSIGN expression
    { $$ = node("expression", 3, $1, $2, $3); }
    | simple-expression
    { $$ = node("expression", 1, $1); }
    ;

var
    : ID
    { $$ = node("var", 1, $1); }
    | ID LBRACKET expression RBRACKET        /* array element */
    { $$ = node("var", 4, $1, $2, $3, $4); }
    ;

simple-expression
    : additive-expression relop additive-expression
    { $$ = node("simple-expression", 3, $1, $2, $3); }
    | additive-expression
    { $$ = node("simple-expression", 1, $1); }
    ;

relop
    : LE
    { $$ = node("relop", 1, $1); }
    | LT
    { $$ = node("relop", 1, $1); }
    | GT
    { $$ = node("relop", 1, $1); }
    | GE
    { $$ = node("relop", 1, $1); }
    | EQ
    { $$ = node("relop", 1, $1); }
    | NEQ
    { $$ = node("relop", 1, $1); }
    ;

additive-expression
    : additive-expression addop term
    { $$ = node("additive-expression", 3, $1, $2, $3); }
    | term
    { $$ = node("additive-expression", 1, $1); }
    ;

addop
    : PLUS
    { $$ = node("addop", 1, $1); }
    | MINUS
    { $$ = node("addop", 1, $1); }
    ;

term
    : term mulop factor
    { $$ = node("term", 3, $1, $2, $3); }
    | factor
    { $$ = node("term", 1, $1); }
    ;

mulop
    : MUL
    { $$ = node("mulop", 1, $1); }
    | DIV
    { $$ = node("mulop", 1, $1); }
    ;

factor
    : LPAREN expression RPAREN
    { $$ = node("factor", 3, $1, $2, $3); }
    | var
    { $$ = node("factor", 1, $1); }
    | call
    { $$ = node("factor", 1, $1); }
    | integer
    { $$ = node("factor", 1, $1); }
    | float
    { $$ = node("factor", 1, $1); }
    ;

integer
    : NUM
    { $$ = node("integer", 1, $1); }
    ;

float
    : FLOATNUM
    { $$ = node("float", 1, $1); }
    ;

/* ---------------- Function calls ---------------- */

call
    : ID LPAREN args RPAREN
    { $$ = node("call", 4, $1, $2, $3, $4); }
    ;

args
    : arg-list
    { $$ = node("args", 1, $1); }
    | /* empty */
    { $$ = node("args", 0); }
    ;

arg-list
    : arg-list COMMA expression
    { $$ = node("arg-list", 3, $1, $2, $3); }
    | expression
    { $$ = node("arg-list", 1, $1); }
    ;

%%

/// The error reporting function.
void yyerror(const char * s)
{
    // TO STUDENTS: This is just an example.
    // You can customize it as you like.
    fprintf(stderr, "error at line %d column %d: %s\n", lines, pos_start, s);
}

/// Parse input from file `input_path`, and prints the parsing results
/// to stdout.  If input_path is NULL, read from stdin.
///
/// This function initializes essential states before running yyparse().
syntax_tree *parse(const char *input_path)
{
    if (input_path != NULL) {
        if (!(yyin = fopen(input_path, "r"))) {
            fprintf(stderr, "[ERR] Open input file %s failed.\n", input_path);
            exit(1);
        }
    } else {
        yyin = stdin;
    }

    lines = pos_start = pos_end = 1;
    gt = new_syntax_tree();
    gt->root = NULL;   // so a failed parse prints nothing instead of garbage
    yyrestart(yyin);
    yyparse();
    return gt;
}

/// A helper function to quickly construct a tree node.
///
/// e.g. $$ = node("program", 1, $1);
syntax_tree_node *node(const char *name, int children_num, ...)
{
    syntax_tree_node *p = new_syntax_tree_node(name);
    syntax_tree_node *child;
    if (children_num == 0) {
        child = new_syntax_tree_node("epsilon");
        syntax_tree_add_child(p, child);
    } else {
        va_list ap;
        va_start(ap, children_num);
        for (int i = 0; i < children_num; ++i) {
            child = va_arg(ap, syntax_tree_node *);
            syntax_tree_add_child(p, child);
        }
        va_end(ap);
    }
    return p;
}