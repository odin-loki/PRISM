type internal_printers = {
  expression:            Ast_c.expression -> string;
  decl:                  Ast_c.declaration -> string;
  assignOp:              Ast_c.assignOp -> string;
  binaryOp:              Ast_c.binaryOp -> string;
  arg_list:              Ast_c.argument Ast_c.wrap2 list -> string;
  typ:                   Ast_c.fullType -> string;
  init:                  Ast_c.initialiser -> string;
  newlines:              Ast_c.newlines -> string;
  init_list:             Ast_c.initialiser Ast_c.wrap2 list -> string;
  field:                 Ast_c.field -> string;
  field_list:            Ast_c.field list -> string;
  statement:             Ast_c.statement -> string;
  statement_seq_list:    Ast_c.statement_sequencable list -> string;
  param:                 Ast_c.parameterType -> string;
  param_list:            (Ast_c.parameterType Ast_c.wrap2 list) -> string;
  template_param:        Ast_c.templateParameterType -> string;
  template_param_list:   (Ast_c.templateParameterType Ast_c.wrap2 list) -> string;
  define_param_list:     ((string Ast_c.wrap) Ast_c.wrap2 list) -> string;
  string_fragment_list:  Ast_c.string_fragment list -> string;
  string_format:         Ast_c.string_format -> string;
  attr_arg:              Ast_c.attr_arg -> string;
}

(* alternate pretty printers, used in pycocci_aux *)
val pp_expression:            Ast_c.expression -> string
val pp_decl:                  Ast_c.declaration -> string
val pr_assignOp:              Ast_c.assignOp -> string
val pr_binaryOp:              Ast_c.binaryOp -> string
val pp_arg_list:              Ast_c.argument Ast_c.wrap2 list -> string
val pp_fullType:              Ast_c.fullType -> string
val pp_init:                  Ast_c.initialiser -> string
val pplines:                  Ast_c.newlines -> string
val pp_init_list:             Ast_c.initialiser Ast_c.wrap2 list -> string
val pp_field:                 Ast_c.field -> string
val pp_field_list:            Ast_c.field list -> string
val pp_statement:             Ast_c.statement -> string
val pp_statement_seq_list:    Ast_c.statement_sequencable list -> string
val pp_param:                 Ast_c.parameterType -> string
val pp_param_list:            (Ast_c.parameterType Ast_c.wrap2 list) -> string
val pp_template_param:        Ast_c.templateParameterType -> string
val pp_template_param_list:   (Ast_c.templateParameterType Ast_c.wrap2 list) -> string
val pp_define_param_list:     ((string Ast_c.wrap) Ast_c.wrap2 list) -> string
val pp_string_fragment_list:  Ast_c.string_fragment list -> string
val pp_string_format:         Ast_c.string_format -> string
val pp_attr_arg:              Ast_c.attr_arg -> string
