(* Victor Gambier
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License (GPL)
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * file license.txt for more details.
 *)
open Common

open Ast_c

(*****************************************************************************)
(* This file is inspired by parsing_c/pretty_print_c.ml, but the functions
in this file return a string instead of printing something. They are still
referred to with terms such as "pretty_printers" "pp" "pr" as they are direct
equivalents, and the end goal of all of these functions is also to print. *)
(*****************************************************************************)

let str_opt (pp: 'a -> string) (opt: 'a option) = match opt with
    None -> "None"
  | Some x -> Printf.sprintf "Some(%s)" (pp x)

(*****************************************************************************)
(* Types *)
(*****************************************************************************)

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

(*****************************************************************************)

let mk_pretty_printers =
  let rec redo =

  let pr_elem info = Ast_c.str_of_info info in

  let pp_list2 to_str l = (* no comma case *)
    "[" ^ String.concat ";" (List.map (fun x -> Printf.sprintf "\"%s\"" (to_str x)) l) ^ "]" in

  let elem_list_to_str l =
    pp_list2 pr_elem l in

  let pp_list to_str l =
      let helper l_i = match l_i with (e, opt) ->
        assert (List.length opt <= 1);
        Printf.sprintf "(%s, %s)" (to_str e) (elem_list_to_str opt) in
      pp_list2 helper l in

  let rec pp_expression = fun ((exp, typ), ii) ->

    (match exp, ii with

    | Ident (ident),         []     ->
        Printf.sprintf "Ident(%s)" (pp_name ident)

    (* only a MultiString can have multiple ii *)
    | Constant (MultiString _), is     ->
        let str_is = elem_list_to_str is in
        Printf.sprintf "Constant(MultiString _), %s" str_is

    | Constant (c), [i] ->
      Printf.sprintf "Constant(_), [%s]" (pr_elem i)

    | StringConstant(s,os,w),  [i1;i2] ->
      let str_s = pp_list2 pp_string_fragment s in
      let str_i1 = pr_elem i1 in
      let str_i2 = pr_elem i2 in
      Printf.sprintf "StringConstant(%s,%s,_),[%s;%s]" str_s os str_i1 str_i2

    | FunCall  (e, es),     [i1;i2] ->
        let str_e = pp_expression e in
        let str_i1 = pr_elem i1 in
        let str_es = pp_arg_list es in
        let str_i2 =  pr_elem i2 in
        Printf.sprintf "FunCall(%s, %s), [%s;%s]" str_e str_es str_i1 str_i2

    | CondExpr (e1, e2, e3),    [i1;i2]    ->
        let str_e2 = match e2 with
          None -> "None"
        | Some x -> Printf.sprintf "Some(%s)" (pp_expression x) in
        Printf.sprintf "CondExpr(%s, %s, %s), [%s;%s]"
          (pp_expression e1) (str_e2) (pp_expression e3) (pr_elem i1) (pr_elem i2;)

    | Sequence (e1, e2),          [i]  ->
        Printf.sprintf "Sequence(%s, %s), [%s]" (pp_expression e1) (pp_expression e2) (pr_elem i)

    | Assignment (e1, op, e2),    []  ->
        Printf.sprintf "Assignment(%s, %s, %s), []" (pp_expression e1) (pr_assignOp op) (pp_expression e2)

    | Postfix  (e, op),    [i] ->
        Printf.sprintf "Postfix(%s, %s), [%s]" (pp_expression e) (pr_fixOp op) (pr_elem i)

    | Infix    (e, op),    [i] ->
        Printf.sprintf "Infix(%s, %s), [%s]" (pp_expression e) (pr_fixOp op) (pr_elem i)

    | Unary    (e, op),    [i] ->
        Printf.sprintf "Unary(%s, %s), [%s]" (pp_expression e) (pr_unaryOp op) (pr_elem i)

    | Binary   (e1, op, e2),    [] ->
        let str_e1 = pp_expression e1 in
        let str_op = pr_binaryOp op in
        let str_e2 = pp_expression e2 in
        Printf.sprintf "Binary(%s, %s, %s), []" str_e1 str_op str_e2

    | ArrayAccess (e, es), [i1;i2] ->
        Printf.sprintf "ArrayAccess(%s, %s), [%s;%s]" (pp_expression e) (pp_arg_list es) (pr_elem i1) (pr_elem i2)

    | RecordAccess   (e, name),     [i1] ->
        Printf.sprintf "RecordAccess(%s, %s), [%s]" (pp_expression e) (pp_name name) (pr_elem i1)

    | RecordPtAccess (e, name),     [i1] ->
        Printf.sprintf "RecordPtAccess(%s, %s), [%s]" (pp_expression e) (pp_name name) (pr_elem i1)

    | QualifiedAccess(typ, name),   [i1] ->
        let str_typ = match typ with
          None -> "None"
        | Some x -> Printf.sprintf "Some(%s)" (pp_fullType x) in
        Printf.sprintf "QualifiedAccess(%s, %s), [%s]" (str_typ) (pp_name name) (pr_elem i1)

    | SizeOfExpr  (e),     [i] ->
        Printf.sprintf "SizeOfExpr(%s), [%s]" (pp_expression e) (pr_elem i)

    | SizeOfType  (t),     [i1;i2;i3] ->
        Printf.sprintf "SizeOfType(%s), [%s;%s;%s]" (pp_fullType t) (pr_elem i1) (pr_elem i2) (pr_elem i3)

    | Cast    (t, e),   [i1;i2] ->
        Printf.sprintf "Cast(%s, %s), [%s;%s]" (pp_fullType t) (pp_expression e) (pr_elem i1) (pr_elem i2)

    | StatementExpr (statxs, [ii1;ii2]),  [i1;i2] ->
        let pr_statxs = pp_list2 pp_statement_seq statxs in
        Printf.sprintf "StatementExpr(%s, [%s;%s]), [%s;%s]" (pr_statxs) (pr_elem ii1) (pr_elem ii2) (pr_elem i1) (pr_elem i2)

    | Constructor (t, init), [lp;rp] ->
        Printf.sprintf "Constructor(%s, %s), [%s;%s]" (pp_fullType t) (pp_init init) (pr_elem lp) (pr_elem rp)

    | ParenExpr (e), [i1;i2] ->
        Printf.sprintf "ParenExpr(%s), [%s;%s]" (pp_expression e) (pr_elem i1) (pr_elem i2)

    (* C++ *)
    | CoAwaitYield (t), [i1] ->
        Printf.sprintf "CoAwaitYield(%s), [%s]" (pp_expression t) (pr_elem i1)

    | New   (pp, t, init),    i1::rest ->
        let str_init = match init with
          None -> "None"
          | Some i -> Printf.sprintf "Some(%s)" (pp_arg_list i) in
        Printf.sprintf "New(_, %s, %s), %s::%s" (pp_fullType t) (str_init) (pr_elem i1) (elem_list_to_str rest)

    | Delete(false,t), [i1] ->
        Printf.sprintf "Delete(false,%s), [%s]" (pp_expression t) (pr_elem i1)

    | Delete(true,t), [i1;i2;i3] ->
        Printf.sprintf "Delete(true,%s), [%s;%s;%s]" (pp_expression t) (pr_elem i1) (pr_elem i2) (pr_elem i3)

    | TemplateInst(name,es),[lab;rab] ->
        Printf.sprintf "TemplateInst(%s,%s),[%s;%s]" (pp_expression name) (pp_arg_list es) (pr_elem lab) (pr_elem rab)

    | TupleExpr(init), [] ->
        Printf.sprintf "TupleExpr(%s), []" (pp_init init)

    | Defined name, [i1] ->
        Printf.sprintf "Defined(%s), [%s]" (pp_name name) (pr_elem i1)

    | Defined name, [i1;i2;i3] ->
        Printf.sprintf "Defined(%s), [%s;%s;%s]" (pp_name name) (pr_elem i1) (pr_elem i2) (pr_elem i3)

    | (Ident (_) | Constant _ | StringConstant _ | FunCall (_,_)
    | CondExpr (_,_,_) | Sequence (_,_) | Assignment (_,_,_)
    | Postfix (_,_) | Infix (_,_) | Unary (_,_) | Binary (_,_,_)
    | ArrayAccess (_,_) | RecordAccess (_,_) | RecordPtAccess (_,_) | QualifiedAccess(_,_)
    | SizeOfExpr (_) | SizeOfType (_) | Cast (_,_)
    | StatementExpr (_) | Constructor _
    | ParenExpr (_) | New (_) | Delete (_,_) | TemplateInst(_,_) | TupleExpr(_)
    | CoAwaitYield (_)
    | Defined (_)),_ -> raise (Impossible 142)
    )

  and pr_assignOp (_,ii) =
    let i = Common.tuple_of_list1 ii in
    Ast_c.str_of_info i

  and pr_binaryOp (_,ii) =
    let i = Common.tuple_of_list1 ii in
    Ast_c.str_of_info i

  and pr_unaryOp = function
    Ast_c.GetRef -> "GetRef"
  | Ast_c.GetRefLabel -> "GetRefLabel"
  | Ast_c.DeRef -> "DeRef"
  | Ast_c.UnPlus -> "UnPlus"
  | Ast_c.UnMinus -> "UnMinus"
  | Ast_c.Tilde -> "Tilde"
  | Ast_c.Not -> "Not"

  and pr_fixOp = function
    Ast_c.Dec -> "Dec"
  | Ast_c.Inc -> "Inc"

  and pp_arg_list es = pp_list pp_argument es

  and pp_argument argument =

    let pp_action (ActMisc ii) = elem_list_to_str ii in

    match argument with
    | Left e ->
      Printf.sprintf "Left(%s)" (pp_expression e)
    | Right weird ->
      (match weird with
      | ArgType param -> Printf.sprintf "Right(ArgType(%s))" (pp_param param)
      | ArgAction action -> Printf.sprintf "Right(ArgAction(%s))" (pp_action action))

  and pp_params (ts, (b, iib)) =
    Printf.sprintf "(%s, (b, %s))" (pp_param_list ts) (elem_list_to_str iib)

  and pp_name = function

    | RegularName (s, ii) ->
        Printf.sprintf "RegularName(%s,_)" s (* we only show the string, as ii contains the same information *)

    | Operator(space_needed,op::ii) ->
      Printf.sprintf "Operator(%B,%s)" (space_needed) (elem_list_to_str ([op] @ ii))

    | Operator(space_needed,_) ->
      failwith "pretty print: bad operator"

    | QualName xs ->
      let helper xs_i = match xs_i with (nm, ii2) -> Printf.sprintf "(%s, %s)" (pp_name nm) (elem_list_to_str ii2) in
      let str_xs = pp_list2 helper xs in
      Printf.sprintf "QualName(%s)" str_xs

    | CppConcatenatedName xs ->
      let helper xs_i = match xs_i with ((x,ii1), ii2) -> Printf.sprintf "((_,%s), %s)" (elem_list_to_str ii1) (elem_list_to_str ii2) in
      let str_xs = pp_list2 helper xs in
      Printf.sprintf "CppConcatenatedName(%s)" str_xs

    | CppVariadicName (s, ii) ->
      let str_ii = elem_list_to_str ii in
      Printf.sprintf "CppVariadicName(%s, %s)" s str_ii

    | CppIdentBuilder ((s,iis), xs) ->
      let helper xs_i = match xs_i with ((x,iix), iicomma) -> Printf.sprintf "((_,%s), %s)" (elem_list_to_str iix) (elem_list_to_str iicomma) in
      let str_xs = pp_list2 helper xs in
      let str_iis = elem_list_to_str iis in
      Printf.sprintf "CppIdentBuilder((%s,%s), %s)" s str_iis str_xs

  and pp_string_fragment (e,ii) =
    match (e,ii) with
      ConstantFragment(str), ii ->
        let (i) = Common.tuple_of_list1 ii in
        Printf.sprintf "ConstantFragment(%s), %s" str (pr_elem i)
    | FormatFragment(fmt), ii ->
        let (i) = Common.tuple_of_list1 ii in
        Printf.sprintf "FormatFragment(%s), %s" (pp_string_format fmt) (pr_elem i)

  and pp_string_fragment_list sfl = pp_list2 pp_string_fragment sfl

  and pp_string_format (e,ii) =
    match (e,ii) with
      ConstantFormat(str), ii ->
        let (i) = Common.tuple_of_list1 ii in
        Printf.sprintf "ConstantFormat(%s), %s" str (pr_elem i)

  and pp_statement_seq_list statxs =
    pp_list2 pp_statement_seq statxs

  and pp_statement = fun st ->
    let statement, ii = (Ast_c.get_st_and_ii st) in
    Printf.sprintf "(%s, %s)"
    (match statement with
    | Labeled (Label (name, st)) ->
        Printf.sprintf "Labeled(Label(%s, %s))" (pp_name name) (pp_statement st)
    | Labeled (Case  (e, st)) ->
        Printf.sprintf "Labeled(Case(%s, %s))" (pp_expression e) (pp_statement st)
    | Labeled (CaseRange  (e, e2, st)) ->
        Printf.sprintf "Labeled(CaseRange(%s, %s, %s))"
          (pp_expression e) (pp_expression e2) (pp_statement st)
    | Labeled (Default st) ->
        Printf.sprintf "Labeled(Default(%s))" (pp_statement st)
    | Compound statxs ->
        Printf.sprintf "Compound(%s)" (pp_statement_seq_list statxs)
    | ExprStatement (None) ->
        Printf.sprintf "ExprStatement(None)"
    | ExprStatement (Some e) ->
        Printf.sprintf "ExprStatement(Some(%s))" (pp_expression e)
    | Selection  (If (e, st1, st2)) ->
        Printf.sprintf "Selection(If (%s, %s, %s))"
          (pp_expression e) (pp_statement st1) (pp_statement st2)
    | Selection  (Ifdef_Ite (e, st1, st2)) ->
        Printf.sprintf "Selection(Ifdef_Ite (%s, %s, %s))"
          (pp_expression e) (pp_statement st1) (pp_statement st2)
    | Selection (Ifdef_Ite2 (e, st1, st2, st3)) ->
        Printf.sprintf "Selection(Ifdef_Ite2 (%s, %s, %s, %s))"
          (pp_expression e) (pp_statement st1) (pp_statement st2) (pp_statement st3)
    | Selection (TryCatch (st, cal)) ->
        let helper cal_i = match cal_i with ((param,st), ii) ->
            Printf.sprintf "((%s,%s), %s)" (pp_param param) (pp_statement st) (elem_list_to_str ii) in
        let str_cal = pp_list2 helper cal in
        Printf.sprintf "Selection(TryCatch(%s, %s))" (pp_statement st) (str_cal)
    | Selection  (Switch (e, st)) ->
        Printf.sprintf "Selection(Switch (%s, %s))"
          (pp_expression e) (pp_statement st)
    | Iteration  (While (WhileExp (e), st)) ->
        Printf.sprintf "Iteration(While(WhileExp (%s), %s))"
          (pp_expression e) (pp_statement st)
    | Iteration  (While (WhileDecl (d), st)) ->
        Printf.sprintf "Iteration(While(WhileDecl(%s), %s))"
          (pp_decl d) (pp_statement st)
    | Iteration  (ScopedGuard (es, st)) ->
        Printf.sprintf "Iteration(ScopedGuard(%s, %s))"
          (pp_arg_list es) (pp_statement st)
    | Iteration  (DoWhile (st, e)) ->
        Printf.sprintf "Iteration(DoWhile(%s, %s))"
          (pp_statement st) (pp_expression e)
    | Iteration  (For (first,st)) ->

      let str_first = (match first with
          ForExp ((e1opt,il1),(e2opt,il2),(e3opt, il3)) ->
            assert (il3 = []);
            Printf.sprintf "ForExp((%s,%s),(%s,%s),(%s, %s))" (str_opt pp_expression e1opt) (elem_list_to_str il1) (str_opt pp_expression e2opt) (elem_list_to_str il2) (str_opt pp_expression e3opt) ("[]")
        | ForDecl (decl,(e2opt,il2),(e3opt, il3)) ->
            assert (il3 = []);
            Printf.sprintf "ForDecl(%s,(%s,%s),(%s, %s))"
              (pp_decl decl) (str_opt pp_expression e2opt) (elem_list_to_str il2) (str_opt pp_expression e3opt) (elem_list_to_str il3)
        | ForRange(decl,ini) ->
            Printf.sprintf "ForRange(%s,%s)" (pp_decl decl) (pp_init ini)
            )
        in

        Printf.sprintf "Iteration(For(%s,%s))"
          (str_first) (pp_statement st)

    | Iteration  (MacroIteration (s,es,st)) ->
        Printf.sprintf "Iteration(MacroIteration(%s,%s,%s))"
          (s) (pp_arg_list es) (pp_statement st)
    | Jump (Goto name) ->
        Printf.sprintf "Jump(Goto(%s))" (pp_name name)
    | Jump ((Continue|Break|Return)) ->
        Printf.sprintf "Jump((Continue|Break|Return))"
    | Jump (ReturnExpr e) ->
        Printf.sprintf "Jump(ReturnExpr(%s))" (pp_expression e)
    | Jump (GotoComputed e) ->
        Printf.sprintf "Jump(GotoComputed(%s))" (pp_expression e)
    | Decl decl ->
        Printf.sprintf "Decl(%s)" (pp_decl decl)
    | Asm asmbody ->
        (match ii with
        | [iasm;iopar;icpar;iptvirg] -> ()
        | [iasm;ivolatile;iopar;icpar;iptvirg] -> ()
        | _ -> raise (Impossible 143)
        );
        Printf.sprintf "Asm(%s)" (pp_asmbody asmbody)
    | NestedFunc def ->
        assert (ii = []);
        Printf.sprintf "NestedFunc(%s)" (pp_def def)
    | MacroStmt ->
        Printf.sprintf "MacroStmt"
    | Exec(code) ->
        Printf.sprintf "Exec(%s)" (pp_list2 pp_exec_code code)
    | IfdefStmt1 (ifdefs, xs) ->
          pp_statement_seq (IfdefStmt2 (ifdefs,List.map (fun x -> [StmtElem x]) xs))
    )
    (elem_list_to_str ii)

  and pp_statement_seq = function
    | StmtElem st -> Printf.sprintf "StmtElem(%s)" (pp_statement st)
    | IfdefStmt ifdef -> Printf.sprintf "IfdefStmt(%s)" (pp_ifdef ifdef)
    | CppDirectiveStmt cpp -> Printf.sprintf "CppDirectiveStmt(%s)" (pp_directive cpp)
    | IfdefStmt2 (ifdef, xxs) -> Printf.sprintf "IfdefStmt2(%s)" (pp_ifdef_tree_sequence ifdef xxs)

  (* ifdef XXX elsif YYY elsif ZZZ endif *)
  and pp_ifdef_tree_sequence ifdef xxs =
    Printf.sprintf "%s %s" (pp_list2 pp_ifdef ifdef) (pp_list2 (pp_list2 pp_statement_seq) xxs)

  and pp_asmbody (string_list, colon_list) =

    let str_colon_list =
      pp_list2
      (fun (Colon xs, ii) ->
          Printf.sprintf "(Colon(%s), %s)"
          (pp_list2
          (fun (x,iicomma) ->
            Printf.sprintf "(%s,%s)" 
            (match x with
              | ColonMisc, ii ->
                Printf.sprintf "ColonMisc, %s" (elem_list_to_str ii)
              | ColonExpr e, [istring;iopar;icpar] ->
                (* the following case used to be just raise Impossible, but
                  the code __asm__ __volatile__ ("dcbz 0, %[input]"
                                            ::[input]"r"(&coherence_data[i]));
                  in linux-2.6.34/drivers/video/fsl-diu-fb.c matches this case *)
                Printf.sprintf "ColonExpr(%s), [%s;%s;%s]" (pp_expression e) (pr_elem istring) (pr_elem iopar) (pr_elem icpar)
              | (ColonExpr e), ii ->
                Printf.sprintf "ColonExpr(%s), %s" (pp_expression e) (elem_list_to_str ii))
            (elem_list_to_str iicomma))
          xs)
        (elem_list_to_str ii))
        colon_list in
      Printf.sprintf "(%s, %s)" (elem_list_to_str string_list) (str_colon_list)

  and pp_exec_code = function
    ExecEval name, [colon] ->
      Printf.sprintf "ExecEval(%s), [%s]" (pp_expression name) (pr_elem colon)
  | ExecToken, [tok] ->
      Printf.sprintf "ExecToken, [%s]" (pr_elem tok)
  | _ -> raise (Impossible 144)

  and pp_v_init = function
      NoInit -> Printf.sprintf "NoInit"
    | ValInit (init, il) (*initialiser wrap*) -> Printf.sprintf "ValInit(%s, %s)" (pp_init init) (elem_list_to_str il)

  and pp_name_vinit_option nameopt =
    (str_opt (fun nv -> Printf.sprintf "%s * %s" (pp_name (fst nv)) (pp_v_init (snd nv))) nameopt)

  and pp_field_list fields = pp_list2 pp_field fields

  and pp_field = function

      DeclarationField(FieldDeclList(onefield_multivars,iiptvirg::ifakestart::iisto)) ->

        Printf.sprintf "DeclarationField(FieldDeclList(%s,%s))"
          (pp_list2
          (fun x -> (match x with
            (Simple (storage, attrs, nameopt, typ, endattrs)), iivirg ->
              (* first var cannot have a preceding ',' *)
              assert (List.length iivirg = 0);
              Printf.sprintf "(Simple (_, %s, %s, %s, %s)), %s"
                (pp_attributes attrs) (pp_name_vinit_option nameopt) (pp_fullType typ)
                (pp_attributes endattrs) (elem_list_to_str iivirg)

            | (BitField (nameopt, typ, iidot, expr)), iivirg ->
              (* first var cannot have a preceding ',' *)
              assert (List.length iivirg = 0);
              Printf.sprintf "(BitField (%s, %s, %s, %s)), %s"
              (str_opt pp_name nameopt) (pp_fullType typ) (pr_elem iidot) (pp_expression expr) (elem_list_to_str iivirg)
          ))
          onefield_multivars)
          (elem_list_to_str ([iiptvirg;ifakestart] @ iisto))

    | DeclarationField(FieldDeclList(onefield_multivars,_)) ->
      failwith "wrong number of tokens"

    | MacroDeclField ((s, es, attrs), ii) ->
      Printf.sprintf "MacroDeclField((%s, %s, %s), %s)" (s) (pp_arg_list es) (pp_attributes attrs) (elem_list_to_str ii)

    | MacroDeclFieldInit ((s, es, attrs, ini), ii) ->
      Printf.sprintf "MacroDeclFieldInit((%s, %s, %s, %s), %s)" (s) (pp_arg_list es) (pp_attributes attrs) (pp_init ini) (elem_list_to_str ii)

    | MacroDeclFieldMarker (s, ii) ->
       Printf.sprintf "MacroDeclFieldMarker(%s, %s)" (s) (elem_list_to_str ii)

    | EmptyField iipttvirg_when_emptyfield ->
      Printf.sprintf "EmptyField(%s)" (pr_elem iipttvirg_when_emptyfield)

    | CppDirectiveStruct cpp ->
      Printf.sprintf "CppDirectiveStruct(%s)" (pp_directive cpp)

    | IfdefStruct ifdef ->
      Printf.sprintf "IfdefStruct(%s)" (pp_ifdef ifdef)

    (* C++ *)
    | FunctionField def ->
      Printf.sprintf "FunctionField(%s)" (pp_def def)

    | AccSpec ii ->
      Printf.sprintf "AccSpec(%s)" (elem_list_to_str ii)

    | ConstructDestructField cd ->
      Printf.sprintf "ConstructDestructField(%s)" (pp_construct_destruct cd)

    and pp_base_type = function
        Void -> "Void"
      | IntType intType -> Printf.sprintf "IntType(%s)"
        (match intType with
          CChar -> "CChar"
        | Si (sign, base) -> Printf.sprintf "Si(%s * %s)"
          (match sign with
            Signed -> "Signed"
          | UnSigned -> "UnSigned")
          (match base with CChar2  -> "CChar2"
          | CShort  -> "CShort"
          | CInt  -> "CInt"
          | CLong  -> "CLong"
          | CLongLong -> "CLongLong"
          )
        )
      | FloatType floatType -> Printf.sprintf "FloatType %s"
        (match floatType with
          CFloat   -> "CFloat"
        | CDouble-> "CDouble"
        | CLongDouble-> "CLongDouble"
        | CFloatComplex-> "CFloatComplex"
        | CDoubleComplex-> "CDoubleComplex"
        | CLongDoubleComplex-> "CLongDoubleComplex"
        | CUnknownComplex-> "CUnknownComplex")
      | SizeType -> "SizeType"
      | SSizeType -> "SSizeType"
      | PtrDiffType -> "PtrDiffType"


  and pp_type_qualifier = function (* printing only the keywords is more readable than also printing the boolean record *)
        ({const=const; volatile=volatile; restrict=restrict}, il) -> Printf.sprintf "(_,%s)" (elem_list_to_str il)

  and (pp_fullType: fullType -> string) =
    fun (qu, attr, (ty, iity)) ->

      Printf.sprintf "(%s, %s, (%s,%s))"

      (pp_type_qualifier qu)
      (pp_attributes attr)

      (match ty with
      |	(NoType) -> "(NoType,_)"
      | (Pointer t)                       -> Printf.sprintf "Pointer(%s)" (pp_fullType t)
      | (ParenType t)                     -> Printf.sprintf "ParenType(%s)" (pp_fullType t)
      | (Array (eopt, t))                 -> Printf.sprintf "Array(%s, %s)" (str_opt pp_expression eopt) (pp_fullType t)
      | (FunctionType (returnt, paramst)) -> Printf.sprintf "FunctionType(%s, _))" (pp_fullType returnt)

      | (StructUnion (su, sopt, optfinal, base_classes, fields)) ->
          Printf.sprintf "StructUnion(_, %s, %s, %s, %s)"
            (str_opt (fun s -> s) sopt) (str_opt pr_elem optfinal) (pp_list pp_base_class base_classes) (pp_list2 pp_field fields)

      | (EnumDef  (typ, base, enumt)) ->

          let enumt_helper = (fun ((name, eopt), iicomma) ->
            Printf.sprintf "((%s, %s), %s)"
            (pp_name name)
            (match eopt with
                  None -> "None"
                | Some x -> Printf.sprintf "Some(%s)" ((fun (ieq, e) -> Printf.sprintf "(%s, %s)" (pr_elem ieq) (pp_expression e)) x)
            )
            (elem_list_to_str iicomma)
            ) in

          let str_enumt = pp_list2 enumt_helper enumt in

          Printf.sprintf "EnumDef(%s, %s, %s)"
            (pp_fullType typ) (str_opt pp_fullType base) (str_enumt)

      | (BaseType bt) -> Printf.sprintf "BaseType(%s)" (pp_base_type bt)
      | (StructUnionName (s, structunion)) -> Printf.sprintf "StructUnionName(_, _)"
      | (EnumName  (key, s)) -> Printf.sprintf "EnumName(_, _)"
      | (TypeName (name)) -> Printf.sprintf "TypeName(%s)" (pp_name name)
      | (Decimal(l,p)) -> Printf.sprintf "Decimal(%s,%s)" (pp_expression l) (str_opt pp_expression p)
      | (QualifiedType (typ,name)) -> Printf.sprintf "QualifiedType(%s,%s)" (str_opt pp_fullType typ) (pp_name name)
      | (NamedType (name,typ)) -> Printf.sprintf "NamedType(%s,%s)" (pp_name name) (str_opt pp_fullType typ)
      | (FieldType (t, _, _)) -> Printf.sprintf "FieldType(%s,_,_)" (pp_fullType t)
      | (TypeOfExpr (e)) ->  Printf.sprintf "TypeOfExpr(%s)" (pp_expression e)
      | (TypeOfType (t)) -> Printf.sprintf "TypeOfType(%s)" (pp_fullType t)
      | (AutoType) -> Printf.sprintf "AutoType"
      | (TemplateType(name,es)) -> Printf.sprintf "TemplateType(%s,%s)" (pp_fullType name) (pp_arg_list es)
      )

      (elem_list_to_str iity)

  and pp_param param =
    let {p_namei = nameopt;
	  p_register = (b,iib);
	  p_type=t;
	  p_endattr=endattr} = param in

    Printf.sprintf "{%s; (_,%s); %s; %s}" (str_opt pp_name nameopt) (elem_list_to_str iib) (pp_fullType t) (pp_attributes endattr)

  and pp_decl = function
      | DeclList ((decl_list, has_ender), vfs) ->

        let pp_decli = function
          ({v_namei = var;
            v_type = returnType;
            v_storage = storage;
            v_attr = attrs;
            v_endattr = endattrs;
            },[])
          -> Printf.sprintf "({%s; %s; _; %s; %s},[])" (pp_name_vinit_option var) (pp_fullType returnType) (pp_attributes attrs) (pp_attributes endattrs)
        | (_,x::xs) -> failwith "DeclList: wrong number of elements"
        in

        let str_decl_list = pp_list2 pp_decli decl_list in

        let str_vfs =
          match vfs with
            iivirg::ifakestart::iisto when has_ender -> elem_list_to_str vfs
            | ifakestart::iisto -> elem_list_to_str vfs
            | _ -> failwith "UsingTypename: wrong number of elements"
        in

        Printf.sprintf "DeclList((%s, %B), %s)" (str_decl_list) (has_ender) (str_vfs)

      | MacroDecl ((sto, preattrs, s, es, attrs, true), iis::lp::rp::iiend::ifakestart::iisto) ->
        Printf.sprintf "MacroDecl((_, %s, %s, %s, %s, true), %s)"
          (pp_attributes preattrs) (s) (pp_arg_list es) (pp_attributes attrs) (elem_list_to_str ([iis;lp;rp;iiend;ifakestart] @ iisto))

      | MacroDecl ((sto, preattrs, s, es, attrs, false), iis::lp::rp::ifakestart::iisto) ->
        Printf.sprintf "MacroDecl((_, %s, %s, %s, %s, false), %s)"
          (pp_attributes preattrs) (s) (pp_arg_list es) (pp_attributes attrs) (elem_list_to_str ([iis;lp;rp;ifakestart] @ iisto))

      | MacroDeclInit  ((sto, preattrs, s, es, attrs, ini), iis::lp::rp::eq::iiend::ifakestart::iisto) ->
        Printf.sprintf "MacroDeclInit((_, %s, %s, %s, %s, %s), %s)"
          (pp_attributes preattrs) (s) (pp_arg_list es) (pp_attributes attrs) (pp_init ini) (elem_list_to_str ([iis;lp;rp;eq;iiend;ifakestart] @ iisto))

      | ((MacroDecl _) | (MacroDeclInit _)) ->
        raise (Impossible 145)

  and pp_init (init, iinit) =
  match init, iinit with
      | InitExpr e, [] ->
          Printf.sprintf "InitExpr(%s), []" (pp_expression e)
      | InitList xs, i1::i2::iicommaopt ->
          xs +> List.iter (fun (x, ii) ->
            assert (List.length ii <= 1);
          );
          let helper xs_i = match xs_i with (x, ii) ->
            Printf.sprintf "(%s, %s)" (pp_init x) (elem_list_to_str ii) in
          let str_xs = pp_list2 helper xs in
          Printf.sprintf "InitList(%s), %s" (str_xs) (elem_list_to_str ([i1;i2] @ iicommaopt))
      | InitListNoBrace xs, iicommaopt ->
          xs +> List.iter (fun (x, ii) ->
            assert (List.length ii <= 1);
          );
          let helper xs_i = match xs_i with (x, ii) ->
            Printf.sprintf "(%s, %s)" (pp_init x) (elem_list_to_str ii) in
          let str_xs = pp_list2 helper xs in
          Printf.sprintf "InitListNoBrace(%s), %s" (str_xs) (elem_list_to_str iicommaopt)

      | InitDesignators (xs, initialiser), [i1] -> (* : *)
          Printf.sprintf "InitDesignators(%s, %s), [%s]" (pp_list2 pp_designator xs) (pp_init initialiser) (pr_elem i1)
      (* no use of '=' in the "Old" style *)
      | InitFieldOld (string, initialiser), [i1;i2] -> (* label:   in oldgcc *)
          Printf.sprintf "InitFieldOld(%s, %s), [%s;%s]"
            (string) (pp_init initialiser) (pr_elem i1) (pr_elem i2)
      | InitIndexOld (expression, initialiser), [i1;i2] -> (* [1] in oldgcc *)
          Printf.sprintf "InitIndexOld(%s, %s), [%s;%s]"
            (pp_expression expression) (pp_init initialiser) (pr_elem i1) (pr_elem i2)
      | (InitIndexOld _ | InitFieldOld _ | InitDesignators _
      | InitList _ | InitExpr _
	  ), _ -> raise (Impossible 146)

  and pplines newlines =
    match newlines with
      Ast_c.Keep -> "Keep"
    | Ast_c.Compress -> "Compress"

  and pp_init_list ini = pp_list pp_init ini

  and pp_designator = function
    | DesignatorField (s), [i1; i2] ->
      Printf.sprintf "DesignatorField(%s), [%s; %s]" (s) (pr_elem i1) (pr_elem i2)
    | DesignatorIndex (expression), [i1;i2] ->
      Printf.sprintf "DesignatorIndex(%s), [%s;%s]"
        (pp_expression expression) (pr_elem i1) (pr_elem i2)
    | DesignatorRange (e1, e2), [iocro;iellipsis;iccro] ->
      Printf.sprintf "DesignatorRange(%s, %s), [%s;%s;%s]"
        (pp_expression e1) (pp_expression e2) (pr_elem iccro) (pr_elem iellipsis) (pr_elem iocro)
    | (DesignatorField _ | DesignatorIndex _ | DesignatorRange _
	), _ -> raise (Impossible 147)

(* ---------------------- *)
  and pp_attributes (attrs: Ast_c.attribute list) : string =
    pp_list2 pp_attribute attrs

  and pp_attribute ((e,ii): Ast_c.attribute) : string =
    match (e,ii) with
      Attribute(a), ii  ->
        Printf.sprintf "Attribute(%s), %s" (pp_attr_arg a) (elem_list_to_str ii)
    | GccAttribute(args), ii ->
        Printf.sprintf "GccAttribute(%s), %s" (pp_arg_list args) (elem_list_to_str ii)
    | CxxAttribute(args), ii ->
        Printf.sprintf "CxxAttribute(%s), %s " (pp_arg_list args) (elem_list_to_str ii)
    | CxxAttributeUsing(atnm, args), ii ->
        Printf.sprintf "CxxAttributeUsing(%s, %s), %s" (pp_name atnm) (pp_arg_list args) (elem_list_to_str ii)

  and pp_attr_arg (e,ii) =
    match (e,ii) with
      MacroAttr(a), ii ->
        Printf.sprintf "MacroAttr(_), %s" (elem_list_to_str ii)
    | MacroAttrArgs(attr, args), ii ->
        Printf.sprintf "MacroAttrArgs(%s, %s), %s" (attr) (pp_arg_list args) (elem_list_to_str ii)

(* ---------------------- *)
  and pp_def_start defbis =
    let {f_name = name;
          f_type = (returnt, (paramst, (b, iib)));
          f_storage = sto;
	        f_constr_inherited = constr_inh;
          f_body = statxs;
	  } = defbis in

    Printf.sprintf "{%s; (%s, (%s, (_, %s))); _; _; %s}"
    (pp_name name)
    (pp_fullType returnt)
    (pp_list pp_param paramst)
    (elem_list_to_str iib)
    (pp_statement_seq_list statxs)

  and pp_def def =
    let defbis, ii = def in
    match ii with
    | iifunc1::iifunc2::i1::i2::ifakestart::ifakeend::isto ->
      Printf.sprintf "(%s, %s)" (pp_def_start defbis) (elem_list_to_str ii)
    | _ -> raise (Impossible 148)

  and pp_ifdef ifdef =
    match ifdef with
    | IfdefDirective (ifdef, ii) ->
        Printf.sprintf "IfdefDirective(_, %s)" (elem_list_to_str ii)

  and pp_param_list paramst = pp_list pp_param paramst

  and pp_construct_destruct (cd,ii) =

    let pp_vrtl vrtl = 
      Printf.sprintf "(%B * %s)" ((fst vrtl)) (elem_list_to_str (snd vrtl))
    in

    Printf.sprintf "(%s,%s)"
      (match cd with
      | ConstructorDecl (vrtl, s, paramst, final)  ->
        Printf.sprintf "ConstructorDecl(%s, %s, %s, %s)"
          (pp_vrtl vrtl) (s) (pp_params paramst) (pp_vrtl final)

      | DestructorDecl (vrtl, s, paramst, final)  ->
        Printf.sprintf "DestructorDecl(%s, %s, %s, %s)"
          (pp_vrtl vrtl) (s) (pp_params paramst) (pp_vrtl final)

      | ConstructorDef (vrtl, s, paramst, constr_init, final, body)  ->
        let str_constr_init = (function
          (inits,[i1]) ->
            let pp_init ((name,args),parens) =
              Printf.sprintf "((%s,%s),%s)" (pp_name name) (pp_arg_list args) (elem_list_to_str parens) in
            Printf.sprintf "(%s,[%s])" (pp_list pp_init inits) (pr_elem i1)
          | _ -> "_") in
        Printf.sprintf "ConstructorDef(%s, %s, %s, %s, %s, %s)"
          (pp_vrtl vrtl) (s) (pp_params paramst) (str_constr_init constr_init) (pp_vrtl final) (pp_statement_seq_list body)

      | DestructorDef (vrtl, s, paramst, final, body)  ->
        Printf.sprintf "DestructorDef(%s, %s, %s, %s, %s)"
          (pp_vrtl vrtl) (s) (pp_params paramst) (pp_vrtl final) (pp_statement_seq_list body)
      )
      (elem_list_to_str ii)

  and pp_directive = function
    | Include {i_include = (s, ii);} ->
      Printf.sprintf "Include {(_, %s)}" (elem_list_to_str ii)
    | Define ((s,ii), (defkind, defval)) ->

    let str_defval = match defval with
            DefineExpr e -> Printf.sprintf "DefineExpr(%s)" (pp_expression e)
            | DefineStmt st -> Printf.sprintf "DefineStmt(%s)" (pp_statement st)
            | DefineDoWhileZero ((st,e), ii) ->
                (match ii with
                | [_;_;_;_] -> ()
                | _ -> raise (Impossible 149));
                Printf.sprintf "DefineDoWhileZero((%s,%s), %s)" (pp_statement st) (pp_expression e) (elem_list_to_str ii)
            | DefineFunction def -> Printf.sprintf "DefineFunction(%s)" (pp_def def)
            | DefineType ty -> Printf.sprintf "DefineType(%s)" (pp_fullType ty)
            | DefineAttr a -> Printf.sprintf "DefineAttr(%s)" (pp_attributes a)
            | DefineText (s, ii) -> Printf.sprintf "DefineText(%s, %s)" (s) (elem_list_to_str ii)
            | DefineEmpty -> Printf.sprintf "DefineEmpty"
            | DefineInit ini -> Printf.sprintf "DefineInit(%s)" (pp_init ini)
            | DefineMulti ss ->  Printf.sprintf "DefineMulti(%s)" (pp_list2 pp_statement ss)
            | DefineTodo -> Printf.sprintf "DefineTodo"
    in
    let str_defkind = (match defkind with
    | DefineVar | Undef -> ();
      Printf.sprintf "DefineVar | Undef"
    | DefineFunc (params, ii) ->
      Printf.sprintf "DefineFunc(%s, %s)" (pp_define_param_list params) (elem_list_to_str ii)
    ) in
      Printf.sprintf "Define((%s,%s), (%s, %s))" (s) (elem_list_to_str ii) (str_defkind) (str_defval)

    | Pragma((name,rest), ii) ->
      Printf.sprintf "Pragma((%s,%s), %s)" (pp_name name) (pr_elem rest) (elem_list_to_str ii)

    | OtherDirective (ii) ->
      Printf.sprintf "OtherDirective(%s)" (elem_list_to_str ii)

    | UsingTypename((name,def),ii) ->
      let _ = match ii with
        [_;_;_;_] -> ()
      | [_;_;_] -> ()
      | _ -> failwith "UsingTypename: wrong number of elements" in
      Printf.sprintf "UsingTypename((%s,%s),%s)" (pp_name name) (pp_fullType def) (elem_list_to_str ii)

    | UsingMember(name,ii) ->
      Printf.sprintf "UsingMember(%s,%s)" (pp_name name) (elem_list_to_str ii)

    | UsingNamespace(name,ii) ->
      Printf.sprintf "UsingNamespace(%s,%s)" (pp_name name) (elem_list_to_str ii)

  and pp_define_param_list dparams =
    pp_list2 (fun (s,iis) -> elem_list_to_str iis) dparams

  and pp_base_class (bc,ii) =

    Printf.sprintf "(%s,%s)"
    (match bc with
        ClassName name -> Printf.sprintf "ClassName(%s)" (pp_name name)
      | CPublic name -> Printf.sprintf "CPublic(%s)" (pp_name name)
      | CProtected name -> Printf.sprintf "CProtected(%s)" (pp_name name)
      | CPrivate name -> Printf.sprintf "CPrivate(%s)" (pp_name name)
    )
    (elem_list_to_str ii)

  and pp_template_param_list paramst = pp_list pp_template_param paramst

  and pp_template_param = function
    TypenameOrClassParam((nm,tyopt),ii) ->
        Printf.sprintf "TypenameOrClassParam((%s,%s),%s)" (pp_name nm) (str_opt pp_fullType tyopt) (elem_list_to_str ii)

    | VarNameParam((ty,nm,expopt),ii) ->
      Printf.sprintf "VarNameParam((%s,%s,%s),%s)" (pp_fullType ty) (pp_name nm) (str_opt pp_init expopt) (elem_list_to_str ii)

    | TemplateParam((params,tmp),ii) ->
      Printf.sprintf "TemplateParam((%s,%s),%s)" (pp_template_param_list params) (pp_template_param tmp) (elem_list_to_str ii)

 in

  { expression            = pp_expression;
    decl                  = pp_decl;
    assignOp              = pr_assignOp;
    binaryOp              = pr_binaryOp;
    arg_list              = pp_arg_list;
    typ                   = pp_fullType;
    init                  = pp_init;
    newlines              = pplines;
    init_list             = pp_init_list;
    field                 = pp_field;
    field_list            = pp_field_list;
    statement             = pp_statement;
    statement_seq_list    = pp_statement_seq_list;
    param                 = pp_param;
    param_list            = pp_param_list;
    template_param        = pp_template_param;
    template_param_list   = pp_template_param_list;
    define_param_list     = pp_define_param_list;
    string_fragment_list  = pp_string_fragment_list;
    string_format         = pp_string_format;
    attr_arg              = pp_attr_arg;
  } in
  redo

(*****************************************************************************)

let ppc = mk_pretty_printers

(* internal pretty printers *)
let pp_expression            = ppc.expression
let pp_decl                  = ppc.decl
let pr_assignOp              = ppc.assignOp
let pr_binaryOp              = ppc.binaryOp
let pp_arg_list              = ppc.arg_list
let pp_fullType              = ppc.typ
let pp_init                  = ppc.init
let pplines                  = ppc.newlines
let pp_init_list             = ppc.init_list
let pp_field                 = ppc.field
let pp_field_list            = ppc.field_list
let pp_statement             = ppc.statement
let pp_statement_seq_list    = ppc.statement_seq_list
let pp_param                 = ppc.param
let pp_param_list            = ppc.param_list
let pp_template_param        = ppc.template_param
let pp_template_param_list   = ppc.template_param_list
let pp_define_param_list     = ppc.define_param_list
let pp_string_fragment_list  = ppc.string_fragment_list
let pp_string_format         = ppc.string_format
let pp_attr_arg              = ppc.attr_arg
