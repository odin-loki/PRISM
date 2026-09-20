(* strip modifications and line numbers to allow identifying identical
expressions, eg in a disjunction *)

module Ast = Ast_cocci
module V = Visitor_ast

(* This treats no terms specially, so some identifying information could be
retained at paces not considered by the visitor.  For the purpose of
finding duplicate terms this will just give false negatives, which does not
compromise correctness. *)

let strip =
  let mcode x = Ast.make_mcode (Ast.unwrap_mcode x) in
  let donothing r k e = Ast.make_term(Ast.unwrap(k e)) in
  V.rebuilder {V.rmcode=mcode} {V.rdonothing=donothing} (fun r k e -> k e)

let strip_ident = strip.V.rebuilder_ident
let strip_expression = strip.V.rebuilder_expression
let strip_fragment = strip.V.rebuilder_fragment
let strip_format = strip.V.rebuilder_format
let strip_assignOp = strip.V.rebuilder_assignOp
let strip_binaryOp = strip.V.rebuilder_binaryOp
let strip_fullType = strip.V.rebuilder_fullType
let strip_typeC = strip.V.rebuilder_typeC
let strip_declaration = strip.V.rebuilder_declaration
let strip_field = strip.V.rebuilder_field
let strip_ann_field = strip.V.rebuilder_ann_field
let strip_enumdecl = strip.V.rebuilder_enumdecl
let strip_initialiser = strip.V.rebuilder_initialiser
let strip_parameter = strip.V.rebuilder_parameter
let strip_template_parameter = strip.V.rebuilder_template_parameter
let strip_parameter_list = strip.V.rebuilder_parameter_list
let strip_statement = strip.V.rebuilder_statement
let strip_case_line = strip.V.rebuilder_case_line
let strip_attribute = strip.V.rebuilder_attribute
let strip_attr_arg = strip.V.rebuilder_attr_arg
let strip_rule_elem = strip.V.rebuilder_rule_elem
let strip_top_level = strip.V.rebuilder_top_level
let strip_expression_dots = strip.V.rebuilder_expression_dots
let strip_statement_dots = strip.V.rebuilder_statement_dots
let strip_anndecl_dots = strip.V.rebuilder_anndecl_dots
let strip_annfield_dots = strip.V.rebuilder_annfield_dots
let strip_enumdecl_dots = strip.V.rebuilder_enumdecl_dots
let strip_initialiser_dots = strip.V.rebuilder_initialiser_dots
let strip_define_param_dots = strip.V.rebuilder_define_param_dots
let strip_define_param = strip.V.rebuilder_define_param
let strip_define_parameters = strip.V.rebuilder_define_parameters
let strip_anything = strip.V.rebuilder_anything

