(*
 * This file is part of Coccinelle, licensed under the terms of the GPL v2.
 * See copyright.txt in the Coccinelle source code for more information.
 * The Coccinelle source code can be obtained at https://coccinelle.gitlabpages.inria.fr/website
 *)

module Ast = Ast_cocci

(*****************************************************************************)
(* This file is inspired by parsing_cocci/pretty_print_cocci.ml, but the
functions in this file return a string instead of printing something. Some are
still referred to with terms such as "print_" as they are direct equivalents,
and the end goal of all of these functions is also to print. In the output,
we do not distinguish between 'x' and 'Ast.unwrap x' for clarity's sake. *)
(*****************************************************************************)

let str_opt (pp: 'a -> string) (opt: 'a option) = match opt with
    None -> "None"
  | Some x -> Printf.sprintf "Some(%s)" (pp x)

let str_list to_str l =
  "[" ^ String.concat ";" (List.map (fun x -> Printf.sprintf "\"%s\"" (to_str x)) l) ^ "]"

let quoted_string s = "\"" ^ s ^ "\""

(* --------------------------------------------------------------------- *)

(* avoid polyvariance problems *)
let anything : (Ast.anything -> string) ref = ref (function _ -> "")

let rec print_anything stream = str_list print_anything_list stream

and print_anything_list l = str_list !anything l

let print_around = function
    Ast.NOTHING -> "NOTHING"
  | Ast.BEFORE(bef,_) -> Printf.sprintf "BEFORE(%s,_)" (print_anything bef)
  | Ast.AFTER(aft,_) -> Printf.sprintf "AFTER(%s,_)" (print_anything aft)
  | Ast.BEFOREAFTER(bef,aft,_) -> Printf.sprintf "BEFOREAFTER(%s,%s,_)" (print_anything bef) (print_anything aft)

let str_info info =
    let to_str = function
      Ast.Noindent s -> Printf.sprintf "Noindent(%s)" s
    | Ast.Indent s -> Printf.sprintf "Indent(%s)" s
    | Ast.Space s -> Printf.sprintf "Space(%s)" s in
    Printf.sprintf "{_;_;%s;%s;_}" (str_list (function (s,_,_) -> to_str s) info.Ast.strbef) (str_list (function (s,_,_) -> to_str s) info.Ast.straft)

let print_meta (_r,x) = x

let print_pos l =
  str_list
  (function
    Ast.MetaPos(name,_,_,_,_) ->
      Printf.sprintf "MetaPos(%s,_,_,_,_)"
      (let name = Ast.unwrap_mcode name in print_meta name)
    | Ast.MetaCom(name,_,_,_) ->
      Printf.sprintf "MetaCom(%s,_,_,_)"
      (let name = Ast.unwrap_mcode name in print_meta name))
  l

let mcode to_str = function
    (x, _, Ast.MINUS(_,_,_adj,plus_stream), pos) ->
      Printf.sprintf "(%s,_,MINUS(_,_,_,%s),_)"
      (to_str x)
      (match plus_stream with
        Ast.NOREPLACEMENT -> "NOREPLACEMENT"
      | Ast.REPLACEMENT(plus_stream,_) -> Printf.sprintf "REPLACEMENT(%s,_)" (print_anything plus_stream))
      (*(print_pos pos)*)
  | (x, _, Ast.CONTEXT(_,Ast.NOTHING), pos) ->
      Printf.sprintf "(%s,_,_,_)"
      (to_str x)
  | (x, _, Ast.CONTEXT(_,plus_streams), pos) ->
      Printf.sprintf "(%s,_,CONTEXT(_,%s),_)"
      (to_str x)
      (print_around plus_streams)
      (*(print_pos pos)*)
  | (x, info, Ast.PLUS _, pos) ->
      Printf.sprintf "(%s,_,_,_)"
      (to_str x)
      (*(str_info info)*)
      (*(print_pos pos)*)

(* --------------------------------------------------------------------- *)
(* Dots *)

let dots to_str d = str_list to_str (Ast.unwrap d)

(* --------------------------------------------------------------------- *)
(* Identifier *)

let rec ident i =
  match Ast.unwrap i with
    Ast.Id(name) -> Printf.sprintf "Id(%s)" (mcode quoted_string name)
  | Ast.MetaId(name,_,keep,inherited) -> Printf.sprintf "MetaId(%s,_,_,_)" (mcode print_meta name)
  | Ast.MetaFunc(name,_,_,_) -> Printf.sprintf "MetaFunc(%s,_,_,_)" (mcode print_meta name)
  | Ast.MetaLocalFunc(name,_,_,_) -> Printf.sprintf "MetaLocalFunc(%s,_,_,_)" (mcode print_meta name)
  | Ast.AsIdent(id,asid) -> Printf.sprintf "AsIdent(%s,%s)" (ident id) (ident asid)
  | Ast.DisjId(id_list) -> Printf.sprintf "DisjId(%s)" (str_list ident id_list)
  | Ast.ConjId(id_list) -> Printf.sprintf "ConjId(%s)" (str_list ident id_list)
  | Ast.OptIdent(id) -> Printf.sprintf "OptIdent(%s)" (ident id)

(* --------------------------------------------------------------------- *)
(* Expression *)

let rec expression e =
  match Ast.unwrap e with
    Ast.Ident(id) -> Printf.sprintf "Ident(%s)" (ident id)
  | Ast.Constant(const) -> Printf.sprintf "Constant(%s)" (mcode constant const)
  | Ast.StringConstant(lq,str,rq,sz) ->
      Printf.sprintf "StringConstant(%s,%s,%s,%s)"
        (mcode quoted_string lq)
        (dots string_fragment str)
        (mcode quoted_string rq)
        (sz2c sz)
  | Ast.FunCall(fn,lp,args,rp) ->
      Printf.sprintf "FunCall(%s,%s,%s,%s)"
        (expression fn)
        (mcode quoted_string lp)
        (dots expression args)
        (mcode quoted_string rp)
  | Ast.Assignment(left,op,right,simple) ->
      Printf.sprintf "Assignment(%s,%s,%s,_)"
        (expression left)
        (assignOp op)
        (expression right)
  | Ast.Sequence(left,op,right) ->
      Printf.sprintf "Sequence(%s,%s,%s)"
        (expression left)
        (mcode quoted_string op)
        (expression right)
  | Ast.CondExpr(exp1,why,exp2,colon,exp3) ->
      Printf.sprintf "CondExpr(%s,%s,%s,%s,%s)"
        (expression exp1)
        (mcode quoted_string why)
        (str_opt (function e ->  expression e) exp2)
        (mcode quoted_string colon)
        (expression exp3)
  | Ast.Postfix(exp,op) ->
      Printf.sprintf "Postfix(%s,%s)"
        (expression exp)
        (mcode fixOp op)
  | Ast.Infix(exp,op) ->
      Printf.sprintf "Infix(%s,%s)"
        (expression exp)
        (mcode fixOp op)
  | Ast.Unary(exp,op) ->
      Printf.sprintf "Unary(%s,%s)"
        (expression exp)
        (mcode unaryOp2c op)
  | Ast.Binary(left,op,right) ->
      Printf.sprintf "Binary(%s,%s,%s)"
        (expression left)
        (binaryOp op)
        (expression right)
  | Ast.Nested(left,op,right) ->
      Printf.sprintf "Nested(%s,%s,%s)"
        (expression left)
        (binaryOp op)
        (expression right)
  | Ast.Paren(lp,exp,rp) ->
      Printf.sprintf "Paren(%s,%s,%s)"
        (mcode quoted_string lp)
        (expression exp)
        (mcode quoted_string rp)
  | Ast.ArrayAccess(fn,lb,args,rb) ->
      Printf.sprintf "ArrayAccess(%s,%s,%s,%s)"
        (expression fn)
        (mcode quoted_string lb)
        (dots expression args)
        (mcode quoted_string rb)
  | Ast.RecordAccess(exp,pt,field) ->
      Printf.sprintf "RecordAccess(%s,%s,%s)"
        (expression exp)
        (mcode quoted_string pt)
        (ident field)
  | Ast.RecordPtAccess(exp,ar,field) ->
      Printf.sprintf "RecordPtAccess(%s,%s,%s)"
        (expression exp)
        (mcode quoted_string ar)
        (ident field)
  | Ast.QualifiedAccess(ty,coloncolon,field) ->
      Printf.sprintf "QualifiedAccess(%s,%s,%s)"
        (str_opt fullType ty)
        (mcode quoted_string coloncolon)
        (ident field)
  | Ast.Cast(lp,ty,rp,exp) ->
      Printf.sprintf "Cast(%s,%s,%s,%s)"
        (mcode quoted_string lp)
        (fullType ty)
        (mcode quoted_string rp)
        (expression exp)
  | Ast.SizeOfExpr(sizeof,exp) ->
      Printf.sprintf "SizeOfExpr(%s,%s)"
        (mcode quoted_string sizeof)
        (expression exp)
  | Ast.SizeOfType(sizeof,lp,ty,rp) ->
      Printf.sprintf "SizeOfType(%s,%s,%s,%s)"
        (mcode quoted_string sizeof)
        (mcode quoted_string lp)
        (fullType ty)
        (mcode quoted_string rp)
  | Ast.CoAwaitYield(yld,exp) ->
      Printf.sprintf "CoAwaitYield(%s,%s)"
        (mcode quoted_string yld)
        (expression exp)
  | Ast.Delete(dlt,exp) ->
      Printf.sprintf "Delete(%s,%s)"
        (mcode quoted_string dlt)
        (expression exp)
  | Ast.DeleteArr(dlt,lb,rb,exp) ->
      Printf.sprintf "DeleteArr(%s,%s,%s,%s)"
        (mcode quoted_string dlt)
        (mcode quoted_string lb)
        (mcode quoted_string rb)
        (expression exp)
  | Ast.New(nw,pp_opt,lp_opt,ty,rp_opt,args_opt) ->
      Printf.sprintf "New(%s,%s,%s,%s,%s,%s)"
        (mcode quoted_string nw)
        (str_opt print_args pp_opt)
        (str_opt (function e -> mcode quoted_string e) lp_opt)
        (fullType ty)
        (str_opt (function e -> mcode quoted_string e) rp_opt)
        (str_opt print_args args_opt)
  | Ast.TemplateInst(name,lp,args,rp) ->
      Printf.sprintf "TemplateInst(%s,%s,%s,%s)"
        (expression name)
        (mcode quoted_string lp)
        (dots expression args)
        (mcode quoted_string rp)
  | Ast.TupleExpr(init) -> Printf.sprintf "TupleExpr(%s)" (initialiser init)
  | Ast.TypeExp(ty) -> Printf.sprintf "TypeExp(%s)" (fullType ty)
  | Ast.Constructor(lp,ty,rp,init) ->
      Printf.sprintf "Constructor(%s,%s,%s,%s)"
        (mcode quoted_string lp)
        (fullType ty)
        (mcode quoted_string rp)
        (initialiser init)
  | Ast.MetaErr(name,_,_,_) -> Printf.sprintf "MetaErr(%s,_,_,_)" (mcode print_meta name)
  | Ast.MetaExpr(name,_,keep,ty,form,inherited,_) ->
      Printf.sprintf "MetaExpr(%s,_,_,%s,_,%B,_)"
        (mcode print_meta name)
        (print_types ty)
        inherited
  | Ast.MetaExprList(name,_,_,_,_) ->
      Printf.sprintf "MetaExprList(%s,_,_,_,_)" (mcode print_meta name)
  | Ast.AsExpr(exp,asexp) ->
      Printf.sprintf "AsExpr(%s,%s)"
        (expression exp)
        (expression asexp)
  | Ast.AsSExpr(exp,asstm) ->
      Printf.sprintf "AsSExpr(%s,%s)"
      (expression exp)
      (rule_elem asstm)
  | Ast.EComma(cm) -> 
      Printf.sprintf "EComma(%s)" (mcode quoted_string cm)
  | Ast.DisjExpr(exp_list) -> Printf.sprintf "DisjExpr(%s)" (str_list expression exp_list)
  | Ast.ConjExpr(exp_list) -> Printf.sprintf "ConjExpr(%s)" (str_list expression exp_list)
  | Ast.NestExpr(starter,expr_dots,ender,Some whencode,multi) ->
      Printf.sprintf "NestExpr(%s,%s,%s,Some(%s),_)"
        (mcode quoted_string starter)
        (str_list expression (Ast.unwrap expr_dots))
        (mcode quoted_string ender)
        (expression whencode)
  | Ast.NestExpr(starter,expr_dots,ender,None,multi) ->
      Printf.sprintf "NestExpr(%s,%s,%s,None,_)"
        (mcode quoted_string starter)
        (str_list expression (Ast.unwrap expr_dots))
        (mcode quoted_string ender)
  | Ast.Edots(dots,Some whencode) ->
      Printf.sprintf "Edots(%s,Some(%s))"
        (mcode quoted_string dots)
        (expression whencode)
  | Ast.Edots(dots,None) -> Printf.sprintf "Edots(%s,None)" (mcode quoted_string dots)
  | Ast.OptExp(exp) -> Printf.sprintf "OptExp(%s)" (expression exp)

and print_args (lp,args,rp) =
  Printf.sprintf "(%s,%s,%s)"
    (mcode quoted_string lp)
    (dots expression args)
    (mcode quoted_string rp)

and string_fragment e =
  match Ast.unwrap e with
    Ast.ConstantFragment(str) -> Printf.sprintf "ConstantFragment(%s)" (mcode quoted_string str)
  | Ast.FormatFragment(pct,fmt) ->
    let string_format e =
      match Ast.unwrap e with
        Ast.ConstantFormat(str) -> Printf.sprintf "ConstantFormat(%s)" (mcode quoted_string str)
      | Ast.MetaFormat(name,_,_,_) -> Printf.sprintf "MetaFormat(%s,_,_,_)" (mcode print_meta name) in
      Printf.sprintf "FormatFragment(%s,%s)"
        (mcode quoted_string pct)
        (string_format fmt)
  | Ast.Strdots dots -> Printf.sprintf "Strdots(%s)" (mcode quoted_string dots)
  | Ast.MetaFormatList(pct,name,lenname,_,_,_) ->
      Printf.sprintf "MetaFormatList(%s,%s,_,_,_,_)"
        (mcode quoted_string pct)
        (mcode print_meta name)

and unaryOp2c = function
    Ast.GetRef -> "GetRef"
  | Ast.GetRefLabel -> "GetRefLabel"
  | Ast.DeRef -> "DeRef"
  | Ast.UnPlus -> "UnPlus"
  | Ast.UnMinus -> "UnMinus"
  | Ast.Tilde s -> Printf.sprintf "Tilde(%s)" s
  | Ast.Not s -> Printf.sprintf "Not(%s)" s

and assignOp op =
  match Ast.unwrap op with
    Ast.SimpleAssign op -> Printf.sprintf "SimpleAssign(%s)" (mcode quoted_string op)
  | Ast.OpAssign(aop) -> Printf.sprintf "OpAssign(%s)" (mcode simple_arithOp aop)
  | Ast.MetaAssign(metavar,_,_,_) -> Printf.sprintf "MetaAssign(%s,_,_,_)" (mcode print_meta metavar)

and fixOp = function
    Ast.Dec -> "Dec"
  | Ast.Inc -> "Inc"

and binaryOp op =
  match Ast.unwrap op with
    Ast.Arith(aop) -> Printf.sprintf "Arith(%s)" (mcode simple_arithOp aop)
  | Ast.Logical(lop) -> Printf.sprintf "Logical(%s)" (mcode logicalOp lop)
  | Ast.MetaBinary(metavar,_,_,_) -> Printf.sprintf "MetaBinary(%s,_,_,_)" (mcode print_meta metavar)

and simple_arithOp = function
    Ast.Plus -> "Plus"
  | Ast.Minus -> "Minus"
  | Ast.Mul -> "Mul"
  | Ast.Div -> "Div"
  | Ast.Min -> "Min"
  | Ast.Max -> "Max"
  | Ast.Mod -> "Mod"
  | Ast.DecLeft -> "DecLeft"
  | Ast.DecRight -> "DecRight"
  | Ast.And s -> Printf.sprintf "And(%s)" s
  | Ast.Or s -> Printf.sprintf "Or(%s)" s
  | Ast.Xor s -> Printf.sprintf "Xor(%s)" s

and logicalOp = function
    Ast.Inf -> "Inf"
  | Ast.Sup -> "Sup"
  | Ast.InfEq -> "InfEq"
  | Ast.SupEq -> "SupEq"
  | Ast.Eq -> "Eq"
  | Ast.NotEq s -> Printf.sprintf "NotEq(%s)" s
  | Ast.AndLog s -> Printf.sprintf "AndLog(%s)" s
  | Ast.OrLog s -> Printf.sprintf "OrLog(%s)" s

and constant = function
    Ast.String(s,sz) -> Printf.sprintf "String(%s,%s)" s (sz2c sz)
  | Ast.Char(s,sz) -> Printf.sprintf "Char(%s,%s)" s (sz2c sz) 
  | Ast.Int(s) -> Printf.sprintf "Int(%s)" s
  | Ast.Float(s) -> Printf.sprintf "Float(%s)" s
  | Ast.DecimalConst(s,_,_) -> Printf.sprintf "DecimalConst(%s,_,_)" s

and sz2c = function
    Ast.IsChar -> "IsChar"
  | Ast.IsUchar -> "IsUchar"
  | Ast.Isuchar -> "Isuchar"
  | Ast.Isu8char -> "Isu8char"
  | Ast.IsWchar -> "IsWchar"

(* --------------------------------------------------------------------- *)
(* Declarations *)

and storage = function
    Ast.Static -> "Static "
  | Ast.Auto -> "Auto "
  | Ast.Register -> "Register "
  | Ast.Extern -> "Extern "

(* --------------------------------------------------------------------- *)
(* Types *)

and fullType ft =
  match Ast.unwrap ft with
    Ast.Type(_,cvbefore,ty,cvafter) ->
      let do_cvattr = function
        Ast.CV cv -> Printf.sprintf "CV(%s)" (mcode const_vol cv)
      | Ast.Attr attr -> Printf.sprintf "Attr(%s)" (print_attribute attr) in
      Printf.sprintf "Type(_,%s,%s,%s)" (str_list do_cvattr cvbefore) (typeC ty) (str_list do_cvattr cvafter)
  | Ast.AsType(ty,asty) -> Printf.sprintf "AsType(%s,%s)" (fullType ty) (fullType asty)
  | Ast.DisjType(decls) -> Printf.sprintf "DisjType(%s)" (str_list fullType decls)
  | Ast.ConjType(decls) -> Printf.sprintf "ConjType(%s)" (str_list fullType decls)
  | Ast.OptType(ty) -> Printf.sprintf "OptType(%s)" (fullType ty)

and print_types = function
    None -> "None"
  | Some l -> str_list fullType l

and print_fninfo = function
    Ast.FStorage(stg) -> Printf.sprintf "FStorage(%s)" (mcode storage stg)
  | Ast.FType(ty) -> Printf.sprintf "FType(%s)" (fullType ty)
  | Ast.FInline(inline) -> Printf.sprintf "FInline(%s)" (mcode quoted_string inline)

and print_attribute_list attrs =
  str_list print_attribute attrs

and print_attribute attr =
  match Ast.unwrap attr with
    Ast.Attribute(a) -> Printf.sprintf "Attribute(%s)" (print_attr_arg a)
  | Ast.GccAttribute(attr_,lp1,lp2,args,rp1,rp2) ->
      Printf.sprintf "GccAttribute(%s,%s,%s,%s,%s,%s)"
      (mcode quoted_string attr_)
      (mcode quoted_string lp1)
      (mcode quoted_string lp2)
      (dots expression args)
      (mcode quoted_string rp1)
      (mcode quoted_string rp2)
  | Ast.CxxAttribute(lb1,args,rb1,rb2) ->
      Printf.sprintf "CxxAttribute(%s,%s,%s,%s)"
      (mcode quoted_string lb1)
      (dots expression args)
      (mcode quoted_string rb1)
      (mcode quoted_string rb2)
  | Ast.CxxAttributeUsing(lb1,usng,atnm,dotdot,args,rb1,rb2) ->
      Printf.sprintf "CxxAttributeUsing(%s,%s,%s,%s,%s,%s,%s)"
      (mcode quoted_string lb1)
      (mcode quoted_string usng)
      (ident atnm)
      (mcode quoted_string dotdot)
      (dots expression args)
      (mcode quoted_string rb1)
      (mcode quoted_string rb2)

and print_attr_arg arg =
  match Ast.unwrap arg with
    Ast.MacroAttr(arg) -> Printf.sprintf "MacroAttr(%s)" (mcode quoted_string arg)
  | Ast.MetaAttr(name,_,_,_) -> Printf.sprintf "MetaAttr(%s,_,_,_)" (mcode print_meta name)
  | Ast.MacroAttrArgs(attr,lp,args,rp) ->
      Printf.sprintf "MacroAttrArgs(%s,%s,%s,%s)"
      (mcode quoted_string attr)
      (mcode quoted_string lp)
      (dots expression args)
      (mcode quoted_string rp)

and typeC ty =
  match Ast.unwrap ty with
    Ast.BaseType(ty,strings) ->
      Printf.sprintf "BaseType(%s,_)"
        (baseType ty)
        (* (str_list (mcode quoted_string) strings) would be too verbose *)
  | Ast.SignedT(sgn,ty) ->
      Printf.sprintf "SignedT(%s,%s)"
        (mcode sign sgn)
        (str_opt typeC ty)
  | Ast.Pointer(ty,star) ->
      Printf.sprintf "Pointer(%s,%s)"
        (fullType ty)
        (mcode unaryOp2c star)
  | Ast.ParenType(lp,ty,rp) ->
      Printf.sprintf "ParenType(%s,%s,%s)"
        (mcode quoted_string lp)
        (fullType ty)
        (mcode quoted_string rp)
  | Ast.FunctionType(ty,lp,params,rp) ->
      Printf.sprintf "FunctionType(%s,%s,%s,%s)"
        (fullType ty)
        (mcode quoted_string lp)
        (parameter_list params)
        (mcode quoted_string rp)
  | Ast.Array(ty,lb,size,rb) ->
      Printf.sprintf "Array(%s,_,%s,_)"
        (fullType ty)
        (*(mcode quoted_string lb)*)
        (str_opt expression size)
        (*(mcode quoted_string rb)*)
  | Ast.Decimal(dec,lp,length,comma,precision_opt,rp) ->
      Printf.sprintf "Decimal(%s,%s,%s,%s,%s,%s)"
        (mcode quoted_string dec)
        (mcode quoted_string lp)
        (expression length)
        (str_opt (mcode quoted_string) comma)
        (str_opt expression precision_opt)
        (mcode quoted_string rp)
  | Ast.EnumName(kind,key,name) ->
      Printf.sprintf "EnumName(%s,%s,%s)"
        (mcode quoted_string kind)
        (str_opt (mcode structUnion) key)
        (str_opt ident name)
  | Ast.EnumDef(ty,base,lb,ids,rb) ->
      Printf.sprintf "EnumDef(%s,%s,%s,%s,%s)"
        (fullType ty)
        (str_opt enum_base base)
        (mcode quoted_string lb)
        (dots enum_decl ids)
        (mcode quoted_string rb)
  | Ast.StructUnionName(kind,name) ->
      Printf.sprintf "StructUnionName(%s,%s)"
        (mcode structUnion kind)
        (str_opt ident name)
  | Ast.StructUnionDef(ty,lb,decls,rb) ->
      Printf.sprintf "StructUnionDef(%s,%s,%s,%s)"
        (fullType ty)
        (mcode quoted_string lb)
        (dots annotated_field decls)
        (mcode quoted_string rb)
  | Ast.TypeName(typename,name) ->
      Printf.sprintf "TypeName(%s,%s)"
        (mcode quoted_string typename)
        (ident name)
  | Ast.TypeOfExpr(typeof,lp,exp,rp) ->
      Printf.sprintf "TypeOfExpr(%s,%s,%s,%s)"
        (mcode quoted_string typeof)
        (mcode quoted_string lp)
        (expression exp)
        (mcode quoted_string rp)
  | Ast.TypeOfType(typeof,lp,ty,rp) ->
      Printf.sprintf "TypeOfType(%s,%s,%s,%s)"
        (mcode quoted_string typeof)
        (mcode quoted_string lp)
        (fullType ty)
        (mcode quoted_string rp)
  | Ast.QualifiedType(ty,coloncolon,name) ->
      Printf.sprintf "QualifiedType(%s,%s,%s)"
        (str_opt fullType ty)
        (mcode quoted_string coloncolon)
        (ident name)
  | Ast.NamedType(name) -> Printf.sprintf "NamedType(%s)" (mcode quoted_string name)
  | Ast.AutoType(auto) -> Printf.sprintf "AutoType(%s)" (mcode quoted_string auto)
  | Ast.MetaType(name,_,_,_) -> Printf.sprintf "MetaType(%s,_,_,_)" (mcode print_meta name)
  | Ast.TemplateType(name,lp,args,rp) ->
      Printf.sprintf "TemplateType(%s,%s,%s,%s)"
        (fullType name)
        (mcode quoted_string lp)
        (dots expression args)
        (mcode quoted_string rp)

and baseType (bt: Ast.baseType) : string = match bt with
    Ast.VoidType -> "VoidType"
  | Ast.CharType -> "CharType"
  | Ast.ShortType -> "ShortType"
  | Ast.ShortIntType -> "ShortIntTypet"
  | Ast.IntType -> "IntType"
  | Ast.DoubleType -> "DoubleType"
  | Ast.LongDoubleType -> "LongDoubleType"
  | Ast.FloatType -> "FloatType"
  | Ast.LongDoubleComplexType -> "LongDoubleComplexType"
  | Ast.DoubleComplexType -> "DoubleComplexType"
  | Ast.FloatComplexType -> "FloatComplexType"
  | Ast.LongType -> "LongType"
  | Ast.LongIntType -> "LongIntType"
  | Ast.LongLongType -> "LongLongType"
  | Ast.LongLongIntType -> "LongLongIntType"
  | Ast.SizeType -> "SizeType"
  | Ast.SSizeType -> "SSizeType"
  | Ast.PtrDiffType -> "PtrDiffType"
  | Ast.BoolType -> "BoolType"
  | Ast.Unknown -> "Unknown"

and structUnion = function
    Ast.Struct -> "Struct"
  | Ast.Union -> "Union"
  | Ast.Class -> "Class"

and sign = function
    Ast.Signed -> "Signed"
  | Ast.Unsigned -> "Unsigned"

and const_vol = function
    Ast.Const -> "Const"
  | Ast.Volatile -> "Volatile"

(* --------------------------------------------------------------------- *)
(* Variable declaration *)
(* Even if the Cocci program specifies a list of declarations, they are
   split out into multiple declarations of a single variable each. *)

and declaration d =
  let alignas (Ast.Align(align,lpar,expr,rpar)) =
  Printf.sprintf "Align(%s,%s,%s,%s)"
    (mcode quoted_string align)
    (mcode quoted_string lpar)
    (expression expr)
    (mcode quoted_string rpar) in
  match Ast.unwrap d with
    Ast.MetaDecl(name,_,_,_) ->
      Printf.sprintf "MetaDecl(%s,_,_,_)" (mcode print_meta name)
  | Ast.AsDecl(decl,asdecl) ->
      Printf.sprintf "AsDecl(%s,%s)"
        (declaration decl)
        (declaration asdecl)
  | Ast.Init(al,stg,ty,id,endattr,eq,ini,sem) ->
      Printf.sprintf "Init(%s,%s,%s,%s,%s,%s,%s,%s)"
        (str_opt alignas al)
        (str_opt (mcode storage) stg)
        (fullType ty)
        (ident id)
        (print_attribute_list endattr)
        (mcode quoted_string eq)
        (initialiser ini)
        (str_opt (mcode quoted_string) sem)
  | Ast.UnInit(al,stg,ty,id,endattr,sem) ->
      Printf.sprintf "UnInit(%s,%s,%s,%s,%s,%s)"
        (str_opt alignas al)
        (str_opt (mcode storage) stg)
        (fullType ty)
        (ident id)
        (print_attribute_list endattr)
        (mcode quoted_string sem)
  | Ast.FunProto (fninfo,name,lp1,params,va,rp1,sem) ->
      let varargs = function
      | None -> "None"
      | Some (comma, ellipsis) ->
        Printf.sprintf "Some(%s,%s)"
          (mcode quoted_string comma)
          (mcode quoted_string ellipsis) in
      Printf.sprintf "FunProto (%s,%s,%s,%s,%s,%s,%s)"
        (str_list print_fninfo fninfo)
        (ident name)
        (mcode quoted_string lp1)
        (parameter_list params)
        (varargs va)
        (mcode quoted_string rp1)
        (mcode quoted_string sem)
  | Ast.MacroDecl(stg,preattr,name,lp,args,rp,attr,sem) ->
      Printf.sprintf "MacroDecl(%s,%s,%s,%s,%s,%s,%s,%s)"
        (str_opt (mcode storage) stg)
        (print_attribute_list preattr)
        (ident name)
        (mcode quoted_string lp)
        (dots expression args)
        (mcode quoted_string rp)
        (print_attribute_list attr)
        (mcode quoted_string sem)
  | Ast.MacroDeclInit(stg,preattr,name,lp,args,rp,attr,eq,ini,sem) ->
      Printf.sprintf "MacroDeclInit(%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)"
        (str_opt (mcode storage) stg)
        (print_attribute_list preattr)
        (ident name)
        (mcode quoted_string lp)
        (dots expression args)
        (mcode quoted_string rp)
        (print_attribute_list attr)
        (mcode quoted_string eq)
        (initialiser ini)
        (mcode quoted_string sem)
  | Ast.TyDecl(ty,sem) ->
      Printf.sprintf "TyDecl(%s,%s)"
        (fullType ty)
        (mcode quoted_string sem)
  | Ast.Typedef(stg,ty,id,sem) ->
      Printf.sprintf "Typedef(%s,%s,%s,%s)"
        (mcode quoted_string stg) 
        (fullType ty)
        (typeC id)
        (mcode quoted_string sem)
  | Ast.DisjDecl(decls) -> Printf.sprintf "isjDecl(%s)" (str_list declaration decls)
  | Ast.ConjDecl(decls) -> Printf.sprintf "ConjDecl(%s)" (str_list declaration decls)
  | Ast.OptDecl(decl) -> Printf.sprintf "OptDecl(%s)" (declaration decl)

and annotated_decl d =
  match Ast.unwrap d with
    Ast.DElem(bef,allminus,decl) ->
      Printf.sprintf "DElem(%s,_,%s)" (mcode (function _ -> "_") ((),Ast.no_info,bef,[])) (declaration decl)

(* --------------------------------------------------------------------- *)
(* Field declaration *)

and field d =
  match Ast.unwrap d with
    Ast.MetaField(name,_,_,_) -> Printf.sprintf "MetaField(%s,_,_,_)" (mcode print_meta name)
  | Ast.MetaFieldList(name,_,_,_,_) -> Printf.sprintf "MetaFieldList(%s,_,_,_,_)" (mcode print_meta name)
  | Ast.Field(ty,id,bf,endattr,sem) ->
      let bitfield (c, e) = Printf.sprintf "(%s,%s)" (mcode quoted_string c)	(expression e) in
      Printf.sprintf "Field(%s,%s,%s,%s,%s)"
        (fullType ty)
        (str_opt ident id)
        (str_opt bitfield bf)
        (print_attribute_list endattr)
        (mcode quoted_string sem)
  | Ast.MacroDeclField(name,lp,args,rp,attr,sem) ->
      Printf.sprintf "MacroDeclField(%s,%s,%s,%s,%s,%s)"
        (ident name)
        (mcode quoted_string lp)
        (dots expression args)
        (mcode quoted_string rp)
        (print_attribute_list attr)
        (mcode quoted_string sem)
  | Ast.CppField(di) -> Printf.sprintf "CppField(%s)" (directive di)
  | Ast.AccSpec(decl,dd) ->
      Printf.sprintf "AccSpec(%s,%s)"
        (mcode quoted_string decl)
        (mcode quoted_string dd)

and annotated_field d =
  match Ast.unwrap d with
    Ast.FElem(bef,allminus,decl) ->
      Printf.sprintf "FElem(%s,_,%s)"
      (mcode (function _ -> "_") ((),Ast.no_info,bef,[]))
      (field decl)
  | Ast.Fdots(dots,Some whencode) ->
      Printf.sprintf "Fdots(%s,Some(%s))"
        (mcode quoted_string dots)
        (field whencode)
  | Ast.Fdots(dots,None) ->
      Printf.sprintf "Fdots(%s,None)" (mcode quoted_string dots)
  | Ast.DisjField(decls) ->
      Printf.sprintf "DisjField(%s)" (str_list annotated_field decls)
  | Ast.ConjField(decls) ->
      Printf.sprintf "ConjField(%s)" (str_list annotated_field decls)
  | Ast.OptField(decl) ->
      Printf.sprintf "OptField(%s)" (annotated_field decl)

and enum_decl d =
  match Ast.unwrap d with
    Ast.Enum(name,enum_val) ->
      Printf.sprintf "Enum(%s,%s)"
        (ident name)
        (match enum_val with
          None -> "None"
        | Some(eq,eval) ->
            Printf.sprintf "Some(%s,%s)"
              (mcode quoted_string eq)
              (expression eval))
  | Ast.EnumComma(cm) ->
      Printf.sprintf "EnumComma(%s)" (mcode quoted_string cm)
  | Ast.EnumDots(dots,Some whencode) ->
      Printf.sprintf "EnumDots(%s,Some %s)" (mcode quoted_string dots) (enum_decl whencode)
  | Ast.EnumDots(dots,None) ->
      Printf.sprintf "EnumDots(%s,None)" (mcode quoted_string dots)

and enum_base (td, ty) =
  Printf.sprintf "(%s,%s)" (mcode quoted_string td) (fullType ty)

(* --------------------------------------------------------------------- *)
(* Initialiser *)

and initialiser i =
  match Ast.unwrap i with
    Ast.MetaInit(name,_,_,_) ->
      Printf.sprintf "MetaInit(%s,_,_,_)" (mcode print_meta name)
  | Ast.MetaInitList(name,_,_,_,_) ->
      Printf.sprintf "MetaInitList(%s,_,_,_,_)" (mcode print_meta name)
  | Ast.AsInit(ini,asini) ->
    Printf.sprintf "AsInit(%s,%s)"
      (initialiser ini)
      (initialiser asini)
  | Ast.InitExpr(exp) -> Printf.sprintf "InitExpr(%s)" (expression exp)
  | Ast.ArInitList(lb,initlist,rb) ->
      Printf.sprintf "ArInitList(%s,%s,%s)"
      (mcode quoted_string lb)
      (dots initialiser initlist)
      (mcode quoted_string rb)
  | Ast.StrInitList(allminus,lb,initlist,rb,whencode) ->
      Printf.sprintf "StrInitList(_,%s,%s,%s,%s)"
      (mcode quoted_string lb)
      (str_list initialiser initlist)
      (mcode quoted_string rb)
      (str_list initialiser whencode)
  | Ast.InitGccExt(designators,eq,ini) ->
    let designator = function
      Ast.DesignatorField(dot,id) ->
        Printf.sprintf "DesignatorField(%s,%s)"
          (mcode quoted_string dot)
          (ident id)
    | Ast.DesignatorIndex(lb,exp,rb) ->
        Printf.sprintf "DesignatorIndex(%s,%s,%s)"
          (mcode quoted_string lb)
          (expression exp)
          (mcode quoted_string rb)
    | Ast.DesignatorRange(lb,min,dots,max,rb) ->
        Printf.sprintf "DesignatorRange(%s,%s,%s,%s,%s)"
          (mcode quoted_string lb)
          (expression min)
          (mcode quoted_string dots)
          (expression max)
          (mcode quoted_string rb) in
      Printf.sprintf "InitGccExt(%s,%s,%s)"
        (str_list designator designators)
        (mcode quoted_string eq)
        (initialiser ini)
  | Ast.InitGccName(name,eq,ini) ->
      Printf.sprintf "InitGccName(%s,%s,%s)"
        (ident name)
        (mcode quoted_string eq)
        (initialiser ini)
  | Ast.IComma(comma) -> Printf.sprintf "IComma(%s)" (mcode quoted_string comma)
  | Ast.Idots(dots,Some whencode) ->
      Printf.sprintf "Idots(%s,Some %s)"
        (mcode quoted_string dots)
        (initialiser whencode)
  | Ast.Idots(dots,None) -> Printf.sprintf "Idots(%s,None)" (mcode quoted_string dots)
  | Ast.OptIni(ini) -> Printf.sprintf "OptIni(%s)" (initialiser ini)

(* --------------------------------------------------------------------- *)
(* Parameter *)

and parameterTypeDef p =
  match Ast.unwrap p with
    Ast.Param(ty,Some id,attr) ->
      Printf.sprintf "Param(%s,Some(%s),%s)"
      (fullType ty)
      (ident id)
      (print_attribute_list attr)
  | Ast.Param(ty,None,attr) ->
      Printf.sprintf "Param(%s,None,%s)"
      (fullType ty)
      (print_attribute_list attr)
  | Ast.MetaParam(name,_,_,_) -> Printf.sprintf "MetaParam(%s,_,_,_)" (mcode print_meta name)
  | Ast.MetaParamList(name,_,_,_,_) -> Printf.sprintf "MetaParamList(%s,_,_,_,_)" (mcode print_meta name)
  | Ast.PComma(cm) -> Printf.sprintf "PComma(%s)" (mcode quoted_string cm)
  | Ast.Pdots(dots) -> Printf.sprintf "Pdots(%s)" (mcode quoted_string dots)
  | Ast.OptParam(param) -> Printf.sprintf "OptParam(%s)" (parameterTypeDef param)
  | Ast.AsParam(p,asexp) ->
      Printf.sprintf "AsParam(%s,%s)"
        (parameterTypeDef p)
        (expression asexp)

and templateParameterTypeDef p =
  match Ast.unwrap p with
    Ast.TypenameOrClassParam(tyorcl,id,eqtyopt) ->
      Printf.sprintf "TypenameOrClassParam(%s,%s,%s)"
        (mcode quoted_string tyorcl)
        (ident id)
        (str_opt (fun (eq,ty) -> Printf.sprintf "(%s * %s)" (mcode quoted_string eq) (fullType ty)) eqtyopt)
  | Ast.VarNameParam(ty,id,eqiniopt) ->
      Printf.sprintf "VarNameParam(%s,%s,%s)"
        (fullType ty)
        (ident id)
        (str_opt (fun (eq,ini) -> Printf.sprintf "(%s * %s)" (mcode quoted_string eq) (initialiser ini)) eqiniopt)
  | Ast.TPComma(comma) -> Printf.sprintf "TPComma(%s)" (mcode quoted_string comma)
  | Ast.TPDots(dots) -> Printf.sprintf "TPDots(%s)" (mcode quoted_string dots)

and parameter_list l = dots parameterTypeDef l

(* --------------------------------------------------------------------- *)
(* Top-level code *)

and rule_elem re =
  match Ast.unwrap re with
    Ast.FunHeader(bef,allminus,fninfo,name,lp,params,va,rp,attrs) ->
      Printf.sprintf "FunHeader(%s,_,%s,%s,%s,%s,%s,%s,%s)"
        (mcode (function _ -> "_") ((),Ast.no_info,bef,[]))
        (str_list print_fninfo fninfo)
        (ident name)
        (mcode quoted_string lp)
        (parameter_list params)
        (match va with
        | None -> "None"
        | Some (comma,ellipsis) ->
          Printf.sprintf "Some(%s,%s)"
            (mcode quoted_string comma)
            (mcode quoted_string ellipsis))
        (mcode quoted_string rp)
        (print_attribute_list attrs)
  | Ast.TemplateDefinitionHeader(tmpkw,lab,params,rab) ->
      let template_parameter_list l = dots templateParameterTypeDef l in
      Printf.sprintf "TemplateDefinitionHeader(%s,%s,%s,%s)"
        (mcode quoted_string tmpkw)
        (mcode quoted_string lab)
        (template_parameter_list params)
        (mcode quoted_string rab)
  | Ast.Decl(ann_decl) ->
      Printf.sprintf "Decl(%s)" (annotated_decl ann_decl)
  | Ast.SeqStart(brace) ->
      Printf.sprintf "SeqStart(%s)"
        (mcode quoted_string brace)
  | Ast.SeqEnd(brace) ->
      Printf.sprintf "SeqEnd(%s)"
        (mcode quoted_string brace)
  | Ast.ExprStatement(exp,sem) ->
      Printf.sprintf "ExprStatement(%s,%s)"
        (str_opt expression exp)
        (mcode quoted_string sem)
  | Ast.IfHeader(iff,lp,exp,rp) ->
      Printf.sprintf "IfHeader(%s,%s,%s,%s)"
        (mcode quoted_string iff)
        (mcode quoted_string lp)
        (expression exp) 
        (mcode quoted_string rp)
  | Ast.Else(els) ->
      Printf.sprintf "Else(%s)"
        (mcode quoted_string els)
  | Ast.WhileHeader(whl,lp,cond,rp) ->
      Printf.sprintf "WhileHeader(%s,%s,%s,%s)"
        (mcode quoted_string whl)
        (mcode quoted_string lp)
        (whileinfo cond)
        (mcode quoted_string rp)
  | Ast.DoHeader(d) ->
      Printf.sprintf "DoHeader(%s)"
        (mcode quoted_string d)
  | Ast.WhileTail(whl,lp,exp,rp,sem) ->
      Printf.sprintf "WhileTail(%s,%s,%s,%s,%s)"
        (mcode quoted_string whl)
        (mcode quoted_string lp)
        (expression exp)
        (mcode quoted_string rp)
        (mcode quoted_string sem)
  | Ast.ForHeader(fr,lp,first,rp) ->
      Printf.sprintf "ForHeader(%s,%s,%s,%s)"
        (mcode quoted_string fr)
        (mcode quoted_string lp)
        (forinfo first)
        (mcode quoted_string rp) 
  | Ast.IteratorHeader(nm,lp,args,rp) ->
      Printf.sprintf "IteratorHeader(%s,%s,%s,%s)"
        (ident nm)
        (mcode quoted_string lp)
        (dots expression args) 
        (mcode quoted_string rp) 
  | Ast.ScopedGuardHeader(sg,lp,exps,rp) ->
      Printf.sprintf "ScopedGuardHeader(%s,%s,%s,%s)"
        (mcode quoted_string sg)
        (mcode quoted_string lp)
        (dots expression exps)
        (mcode quoted_string rp)
  | Ast.SwitchHeader(switch,lp,exp,rp) ->
      Printf.sprintf "SwitchHeader(%s,%s,%s,%s)"
        (mcode quoted_string switch)
        (mcode quoted_string lp)
        (expression exp)
        (mcode quoted_string rp) 
  | Ast.Break(br,sem) ->
      Printf.sprintf "Break(%s,%s)"
        (mcode quoted_string br)
        (mcode quoted_string sem)
  | Ast.Continue(cont,sem) ->
      Printf.sprintf "Continue(%s,%s)"
        (mcode quoted_string cont)
        (mcode quoted_string sem)
  | Ast.Label(l,dd) ->
      Printf.sprintf "Label(%s,%s)"
        (ident l)
        (mcode quoted_string dd)
  | Ast.Goto(goto,l,sem) ->
      Printf.sprintf "Goto(%s,%s,%s)"
        (mcode quoted_string goto)
        (ident l)
        (mcode quoted_string sem)
  | Ast.Return(ret,sem) ->
      Printf.sprintf "Return(%s,%s)"
        (mcode quoted_string ret)
        (mcode quoted_string sem)
  | Ast.ReturnExpr(ret,exp,sem) ->
      Printf.sprintf "ReturnExpr(%s,%s,%s)"
        (mcode quoted_string ret)
        (expression exp)
        (mcode quoted_string sem)
  | Ast.Exec(exec,lang,code,sem) ->
      let exec_code e =
        match Ast.unwrap e with
          Ast.ExecEval(colon,id) -> Printf.sprintf "ExecEval(%s,%s)" (mcode quoted_string colon) (expression id)
        | Ast.ExecToken(tok) -> Printf.sprintf "ExecToken(%s)" (mcode quoted_string tok)
        | Ast.ExecDots(dots) -> Printf.sprintf "ExecDots(%s)" (mcode quoted_string dots) in
      Printf.sprintf "Exec(%s,%s,%s,%s)"
        (mcode quoted_string exec)
        (mcode quoted_string lang)
        (dots exec_code code)
        (mcode quoted_string sem)
  | Ast.MetaRuleElem(name,_,_,_) ->
      Printf.sprintf "MetaRuleElem(%s,_,_,_)"
        (mcode print_meta name)
  | Ast.MetaStmt(name,_,_,_,_) ->
      Printf.sprintf "MetaStmt(%s,_,_,_,_)"
        (mcode print_meta name)
  | Ast.MetaStmtList(name,_,_,_,_) ->
      Printf.sprintf "MetaStmtList(%s,_,_,_,_)"
        (mcode print_meta name)
  | Ast.Exp(exp) -> Printf.sprintf "Exp(%s)" (expression exp)
  | Ast.TopExp(exp) -> Printf.sprintf "TopExp(%s)" (expression exp)
  | Ast.Ty(ty) -> Printf.sprintf "Ty(%s)" (fullType ty)
  | Ast.TopId(id) -> Printf.sprintf "TopId(%s)" (ident id)
  | Ast.TopInit(init) -> Printf.sprintf "TopInit(%s)" (initialiser init)
  | Ast.TopAttr attr -> Printf.sprintf "TopAttr(%s)" (print_attribute attr)
  | Ast.CppTop(di) -> Printf.sprintf "CppTop(%s)" (directive di)
  | Ast.Undef(def,id) ->
      Printf.sprintf "Undef(%s,%s)"
        (mcode quoted_string def)
        (ident id)
  | Ast.DefineHeader(def,id,params) ->
      Printf.sprintf "DefineHeader(%s,%s,%s)"
        (mcode quoted_string def)
        (ident id)
        (print_define_parameters params)
  | Ast.Default(def,colon) ->
      Printf.sprintf "Default(%s,%s)"
        (mcode quoted_string def)
        (mcode quoted_string colon)
  | Ast.AsRe(re,asre) ->
      Printf.sprintf "AsRe(%s,%s)"
        (rule_elem re)
        (rule_elem asre)
  | Ast.Case(case,exp,colon) ->
      Printf.sprintf "Case(%s,%s,%s)"
        (mcode quoted_string case)
        (expression exp)
        (mcode quoted_string colon) 
  | Ast.DisjRuleElem(res) ->
      Printf.sprintf "DisjRuleElem(%s)"
        (str_list (rule_elem) res)

and forinfo = function
    Ast.ForExp(e1,sem1,e2,sem2,e3) ->
      Printf.sprintf "ForExp(%s,%s,%s,%s,%s)"
        (str_opt expression e1)
        (mcode quoted_string sem1)
        (str_opt expression e2)
        (mcode quoted_string sem2)
        (str_opt expression e3)
  | Ast.ForDecl(ann_decl,e2,sem2,e3) ->
      Printf.sprintf "ForDecl(%s,%s,%s,%s)"
        (annotated_decl ann_decl)
        (str_opt expression e2)
        (mcode quoted_string sem2)
        (str_opt expression e3)
  | Ast.ForRange(ann_decl, ini) ->
      Printf.sprintf "ForRange(%s, %s)"
        (annotated_decl ann_decl)
        (initialiser ini)

and print_define_parameters params =
  match Ast.unwrap params with
    Ast.NoParams -> "NoParams"
  | Ast.DParams(lp,params,rp) ->
      Printf.sprintf "DParams(%s,%s,%s)"
        (mcode quoted_string lp)
        (dots print_define_param params)
        (mcode quoted_string rp)

and print_define_param param =
  match Ast.unwrap param with
    Ast.DParam(id) -> Printf.sprintf "DParam(%s)" (ident id)
  | Ast.DParamEll(id,dots) -> Printf.sprintf "DParamEll(%s,%s)" (ident id) (mcode quoted_string dots)
  | Ast.MetaDParamList(name,_,_,_,_) -> Printf.sprintf "MetaDParamList(%s,_,_,_,_)" (mcode print_meta name)
  | Ast.DPComma(comma) -> Printf.sprintf "DPComma(%s)" (mcode quoted_string comma)
  | Ast.DPdots(dots) -> Printf.sprintf "DPdots(%s)" (mcode quoted_string dots)
  | Ast.OptDParam(dp) -> Printf.sprintf "OptDParam(%s)" (print_define_param dp)

and statement s =
  match Ast.unwrap s with
    Ast.Seq(lbrace,body,rbrace) ->
      Printf.sprintf "Seq(%s,%s,%s)"
        (rule_elem lbrace)
        (dots statement body)
        (rule_elem rbrace)
  | Ast.IfThen(header,branch,(_,_,_,aft)) ->
      Printf.sprintf "IfThen(%s,%s,(_,_,_,%s))"
        (rule_elem header)
        (statement branch)
        (mcode (function _ -> "_") ((),Ast.no_info,aft,[]))
  | Ast.IfThenElse(header,branch1,els,branch2,(_,_,_,aft)) ->
      Printf.sprintf "IfThenElse(%s,%s,%s,%s,(_,_,_,%s))"
        (rule_elem header)
        (statement branch1)
        (rule_elem els)
        (statement branch2)
        (mcode (function _ -> "_") ((),Ast.no_info,aft,[]))
  | Ast.While(header,body,(_,_,_,aft)) ->
      Printf.sprintf "While(%s,%s,(_,_,_,%s))"
        (rule_elem header)
        (statement body)
        (mcode (function _ -> "_") ((),Ast.no_info,aft,[]))
  | Ast.Do(header,body,tail) ->
      Printf.sprintf "Do(%s,%s,%s)"
        (rule_elem header)
        (statement body)
        (rule_elem tail)
  | Ast.For(header,body,(_,_,_,aft)) ->
      Printf.sprintf "For(%s,%s,(_,_,_,%s))"
        (rule_elem header)
        (statement body)
        (mcode (function _ -> "_") ((),Ast.no_info,aft,[]))
  | Ast.ScopedGuard(header,body,(_,_,_,aft)) ->
      Printf.sprintf "ScopedGuard(%s,%s,(_,_,_,%s))"
        (rule_elem header)
        (statement body)
        (mcode (function _ -> "_") ((),Ast.no_info,aft,[]))
  | Ast.Iterator(header,body,(_,_,_,aft)) ->
      Printf.sprintf "Iterator(%s,%s,(_,_,_,%s))"
        (rule_elem header)
        (statement body)
        (mcode (function _ -> "_") ((),Ast.no_info,aft,[]))
  | Ast.Switch(header,lb,decls,cases,rb) ->
      Printf.sprintf "Switch(%s,%s,%s,%s,%s)"
        (rule_elem header)
        (rule_elem lb)
        (dots statement decls)
        (str_list case_line cases)
        (rule_elem rb)
  | Ast.Atomic(re) ->
      Printf.sprintf "Atomic(%s)" (rule_elem re)
  | Ast.FunDecl(header,lbrace,body,rbrace,(_,_,_,aft)) ->
      Printf.sprintf "FunDecl(%s,%s,%s,%s,(_,_,_,%s))"
        (rule_elem header)
        (rule_elem lbrace)
        (dots statement body)
        (rule_elem rbrace)
        (mcode (function _ -> "_") ((),Ast.no_info,aft,[]))
  | Ast.TemplateDefinition(header,stmt) ->
      Printf.sprintf "TemplateDefinition(%s,%s)"
        (rule_elem header)
        (statement stmt)
  | Ast.Disj([stmt_dots]) ->
      Printf.sprintf "Disj([%s])" (dots statement stmt_dots)
  | Ast.Conj([stmt_dots]) ->
      Printf.sprintf "Conj([%s])" (dots statement stmt_dots)
  | Ast.Disj(stmt_dots_list) ->
      Printf.sprintf "Disj(%s)"
        (str_list (dots statement) stmt_dots_list)
  | Ast.Conj(stmt_dots_list) ->
      Printf.sprintf "Conj(%s)"
        (str_list (dots statement) stmt_dots_list)
  | Ast.Define(header,body) ->
      Printf.sprintf "Define(%s,%s)"
        (rule_elem header)
        (match body with
          Ast.DefineStms body -> Printf.sprintf "DefineStms(%s)" (dots statement body)
        | Ast.DefineAttr attr -> Printf.sprintf "DefineAttr(%s)" (rule_elem attr))
  | Ast.AsStmt(stm,asstm) ->
      Printf.sprintf "AsStmt(%s,%s)"
        (statement stm)
        (statement asstm)
  | Ast.Nest(starter,stmt_dots,ender,whn,multi,_,_) ->
      Printf.sprintf "Nest(%s,%s,%s,%s,_,_,_)"
        (mcode quoted_string starter)
        (str_list statement (Ast.unwrap stmt_dots))
        (mcode quoted_string ender)
        (str_list (whencode (dots statement) statement) whn)
  | Ast.Dots(d,whn,_,_) ->
      Printf.sprintf "Dots(%s,%s,_,_)"
        (mcode quoted_string d)
        (str_list (whencode (dots statement) statement) whn)
  | Ast.OptStm(s) ->
      Printf.sprintf "OptStm(%s)" (statement s)

and whileinfo cond =
  match cond with
    Ast.WhileExp(e) -> Printf.sprintf "WhileExp(%s)" (expression e)
  | Ast.WhileDecl(d) -> Printf.sprintf "WhileDecl(%s)" (annotated_decl d)

and directive di =
  match Ast.unwrap di with
    Ast.Include(inc,s) ->
      Printf.sprintf "Include(%s,%s)" 
        (mcode quoted_string inc)
        (mcode inc_file s)
  | Ast.MetaInclude(inc,s) ->
      Printf.sprintf "MetaInclude(%s,%s)" 
        (mcode quoted_string inc)
        (expression s)
  | Ast.Pragma(prg,id,body) ->
      let pragmainfo pi =
        match Ast.unwrap pi with
          Ast.PragmaString(s) -> Printf.sprintf "PragmaString(%s)" (mcode quoted_string s)
        | Ast.PragmaDots (dots) -> Printf.sprintf "PragmaDots (%s)" (mcode quoted_string dots)
        | Ast.MetaPragmaInfo(metavar,_,_,_) -> Printf.sprintf "MetaPragmaInfo(%s,_,_,_)" (mcode print_meta metavar) in
      Printf.sprintf "Pragma(%s,%s,%s)" 
        (mcode quoted_string prg)
        (ident id) 
        (pragmainfo body)
  | Ast.UsingNamespace(usng,nmspc,name,sem) ->
      Printf.sprintf "UsingNamespace(%s,%s,%s,%s)" 
        (mcode quoted_string usng) 
        (mcode quoted_string nmspc)
        (ident name)
        (mcode quoted_string sem)
  | Ast.UsingTypename(usng,name,eq,tn,ty,sem) ->
      Printf.sprintf "UsingTypename(%s,%s,%s,%s,%s,%s)" 
        (mcode quoted_string usng) 
        (ident name) 
        (mcode quoted_string eq  ) 
        (str_opt (fun x -> mcode quoted_string x) tn)
        (fullType ty) 
        (mcode quoted_string sem)
  | Ast.UsingMember(usng,name,sem) ->
      Printf.sprintf "UsingMember(%s,%s,%s)" 
        (mcode quoted_string usng)
        (ident name) 
        (mcode quoted_string sem)

and whencode notfn alwaysfn = function
    Ast.WhenNot a -> Printf.sprintf "WhenNot(%s)" (notfn a)
  | Ast.WhenAlways a -> Printf.sprintf "WhenAlways(%s)" (alwaysfn a)
  | Ast.WhenModifier x ->
      let print_when_modif = function
      | Ast.WhenAny    -> "WhenAny"
      | Ast.WhenStrict -> "WhenStrict"
      | Ast.WhenForall -> "WhenForall"
      | Ast.WhenExists -> "WhenExists" in
      Printf.sprintf "WhenModifier(%s)" (print_when_modif x)
  | Ast.WhenNotTrue a -> Printf.sprintf "WhenNotTrue(%s)" (rule_elem a)
  | Ast.WhenNotFalse a -> Printf.sprintf "WhenNotFalse(%s)" (rule_elem a)



and case_line c =
  match Ast.unwrap c with
    Ast.CaseLine(header,code) ->
      Printf.sprintf "CaseLine(%s,%s)"
        (rule_elem header)
        (dots statement code)
  | Ast.OptCase(case) ->
      Printf.sprintf "OptCase(%s)" (case_line case)

(* --------------------------------------------------------------------- *)
(* CPP code *)

and inc_file inc_f  =
  let inc_elem  = function
    Ast.IncPath s -> Printf.sprintf "IncPath(%s)" s
  | Ast.IncDots -> "IncDots" in
  match inc_f with
    Ast.Local(elems) -> Printf.sprintf "Local(%s)" (str_list inc_elem elems)
  | Ast.NonLocal(elems) -> Printf.sprintf "NonLocal(%s)" (str_list inc_elem elems)
  | Ast.AnyInc -> "AnyInc"

let print_listlen = function
    Ast.MetaLen((r,n),_) -> Printf.sprintf "MetaLen((%s,%s),_)" r n
  | Ast.CstLen(n) -> Printf.sprintf "CstLen(%i)" n
  | Ast.AnyLen -> "AnyLen"

let print_seed_elem = function
    Ast.SeedString(s) -> Printf.sprintf "SeedString(%s)" s
  | Ast.SeedId(r,n) -> Printf.sprintf "SeedId(%s,%s)" r n

let print_seed = function
    Ast.NoVal -> "NoVal"
  | Ast.StringSeed(s) -> Printf.sprintf "StringSeed(%s)" s
  | Ast.ListSeed(ss) -> Printf.sprintf "ListSeed(%s)" (str_list print_seed_elem ss)
  | Ast.ScriptSeed _ -> "ScriptSeed(_)"

let unparse_cocci_mv  = function
    Ast.MetaMetaDecl _ -> failwith "should be removed"
  | Ast.MetaIdDecl(_,(r,n)) -> Printf.sprintf "MetaIdDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaFreshIdDecl((r,n),seed) -> Printf.sprintf "MetaFreshIdDecl((%s,%s),%s)" (quoted_string r) (quoted_string n) (print_seed seed) 
  | Ast.MetaTypeDecl(_,(r,n)) -> Printf.sprintf "MetaTypeDecl(_,(%s,%s))" (quoted_string r) (quoted_string n) 
  | Ast.MetaInitDecl(_,(r,n)) -> Printf.sprintf "MetaInitDecl(_,(%s,%s))" (quoted_string r) (quoted_string n) 
  | Ast.MetaInitListDecl(_,(r,n),len) -> Printf.sprintf "MetaInitListDecl(_,(%s,%s),%s)" (quoted_string r) (quoted_string n) (print_listlen len) 
  | Ast.MetaListlenDecl(r,n) -> Printf.sprintf "MetaListlenDecl(_,_)"
  | Ast.MetaParamDecl(_,(r,n)) -> Printf.sprintf "MetaParamDecl(_,(%s,%s))" (quoted_string r) (quoted_string n) 
  | Ast.MetaBinaryOperatorDecl(_,(r,n)) -> Printf.sprintf "MetaBinaryOperatorDecl(_,(%s,%s))" (quoted_string r) (quoted_string n) 
  | Ast.MetaAssignmentOperatorDecl(_,(r,n)) -> Printf.sprintf "MetaAssignmentOperatorDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaPragmaInfoDecl(_,(r,n)) -> Printf.sprintf "MetaPragmaInfoDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaParamListDecl(_,(r,n),len) -> Printf.sprintf "MetaParamListDecl(_,(%s,%s),%s)" (quoted_string r) (quoted_string n) (print_listlen len)
  | Ast.MetaConstDecl(_,(r,n),ty) -> Printf.sprintf "MetaConstDecl(_,(%s,%s),%s)" (quoted_string r) (quoted_string n) (print_types ty)
  | Ast.MetaErrDecl(_,(r,n)) -> Printf.sprintf "MetaErrDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaExpDecl(_,(r,n),None,_bitfield) -> Printf.sprintf "MetaExpDecl(_,(%s,%s),None,_)" (quoted_string r) (quoted_string n)
  | Ast.MetaExpDecl(_,(r,n),ty,_bitfield) -> Printf.sprintf "MetaExpDecl(_,(%s,%s),%s,_)" (quoted_string r) (quoted_string n) (print_types ty)
  | Ast.MetaIdExpDecl(_,(r,n),ty) -> Printf.sprintf "MetaIdExpDecl(_,(%s,%s),%s)" (quoted_string r) (quoted_string n) (print_types ty)
  | Ast.MetaLocalIdExpDecl(_,(r,n),ty) -> Printf.sprintf "MetaLocalIdExpDecl(_,(%s,%s),%s)" (quoted_string r) (quoted_string n) (print_types ty)
  | Ast.MetaGlobalIdExpDecl(_,(r,n),ty) -> Printf.sprintf "MetaGlobalIdExpDecl(_,(%s,%s),%s)" (quoted_string r) (quoted_string n) (print_types ty)
  | Ast.MetaExpListDecl(_,(r,n),len) -> Printf.sprintf "MetaExpListDecl(_,(%s,%s),%s)" (quoted_string r) (quoted_string n) (print_listlen len)
  | Ast.MetaDeclDecl(_,(r,n)) -> Printf.sprintf "MetaDeclDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaFieldDecl(_,(r,n)) -> Printf.sprintf "MetaFieldDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaFieldListDecl(_,(r,n),len) -> Printf.sprintf "MetaFieldListDecl(_,(%s,%s),%s)" (quoted_string r) (quoted_string n) (print_listlen len)
  | Ast.MetaStmDecl(_,(r,n)) -> Printf.sprintf "MetaStmDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaStmListDecl(_,(r,n),len) -> Printf.sprintf "MetaStmListDecl(_,(%s,%s),%s)" (quoted_string r) (quoted_string n) (print_listlen len)
  | Ast.MetaDParamListDecl(_,(r,n),len) -> Printf.sprintf "MetaDParamListDecl(_,(%s,%s),%s)" (quoted_string r) (quoted_string n) (print_listlen len)
  | Ast.MetaFuncDecl(_,(r,n)) -> Printf.sprintf "MetaFuncDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaLocalFuncDecl(_,(r,n)) -> Printf.sprintf "MetaLocalFuncDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaPosDecl(_,(r,n)) -> Printf.sprintf "MetaPosDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaComDecl(_,(r,n)) -> Printf.sprintf "MetaComDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaFmtDecl(_,(r,n)) -> Printf.sprintf "MetaFmtDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaAttributeDecl(_,(r,n)) -> Printf.sprintf "MetaAttributeDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaFragListDecl(_,(r,n),len) -> Printf.sprintf "MetaFragListDecl(_,(%s,%s),%s)" (quoted_string r) (quoted_string n) (print_listlen len)
  | Ast.MetaAnalysisDecl(analyzer,(r,n)) -> failwith "analyzer not supported"
  | Ast.MetaDeclarerDecl(_,(r,n)) -> Printf.sprintf "MetaDeclarerDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaIteratorDecl(_,(r,n)) -> Printf.sprintf "MetaIteratorDecl(_,(%s,%s))" (quoted_string r) (quoted_string n)
  | Ast.MetaScriptDecl _ -> failwith "not a cocci decl"

(* --------------------------------------------------------------------- *)

let top_level t =
  match Ast.unwrap t with
    Ast.FILEINFO(old_file,new_file) ->
      Printf.sprintf "FILEINFO(%s,%s)" (mcode quoted_string old_file) (mcode quoted_string new_file)
  | Ast.NONDECL(stmt) ->
      Printf.sprintf "NONDECL(%s)" (statement stmt)
  | Ast.CODE(stmt_dots) ->
      Printf.sprintf "CODE(%s)" (dots statement stmt_dots)
  | Ast.ERRORWORDS(exps) ->
      Printf.sprintf "ERRORWORDS(%s)" (str_list expression exps)

let rule =
  str_list top_level

let _ = anything := function
      Ast.FullTypeTag(x) -> Printf.sprintf "FullTypeTag(%s)" (fullType x)
    | Ast.BaseTypeTag(x) -> Printf.sprintf "BaseTypeTag(%s)" (baseType x)
    | Ast.StructUnionTag(x) -> Printf.sprintf "StructUnionTag(%s)" (structUnion x)
    | Ast.SignTag(x) -> Printf.sprintf "SignTag(%s)" (sign x)
    | Ast.IdentTag(x) -> Printf.sprintf "IdentTag(%s)" (ident x)
    | Ast.ExpressionTag(x) -> Printf.sprintf "ExpressionTag(%s)" (expression x)
    | Ast.ConstantTag(x) -> Printf.sprintf "ConstantTag(%s)" (constant x)
    | Ast.UnaryOpTag(x) -> Printf.sprintf "UnaryOpTag(%s)" (unaryOp2c x)
    | Ast.AssignOpTag(x) -> Printf.sprintf "AssignOpTag(%s)" (assignOp x)
    | Ast.SimpleAssignOpTag(x) -> Printf.sprintf "SimpleAssignOpTag(%s)" (x)
    | Ast.OpAssignOpTag(x) -> Printf.sprintf "OpAssignOpTag(%s)" (simple_arithOp x)
    | Ast.FixOpTag(x) -> Printf.sprintf "FixOpTag(%s)" (fixOp x)
    | Ast.BinaryOpTag(x) -> Printf.sprintf "BinaryOpTag(%s)" (binaryOp x)
    | Ast.ArithOpTag(x) -> Printf.sprintf "ArithOpTag(%s)" (simple_arithOp x)
    | Ast.LogicalOpTag(x) -> Printf.sprintf "LogicalOpTag(%s)" (logicalOp x)
    | Ast.InitTag(x) -> Printf.sprintf "InitTag(%s)" (initialiser x)
    | Ast.DeclarationTag(x) -> Printf.sprintf "DeclarationTag(%s)" (declaration x)
    | Ast.FieldTag(x) -> Printf.sprintf "FieldTag(%s)" (field x)
    | Ast.EnumDeclTag(x) -> Printf.sprintf "EnumDeclTag(%s)" (enum_decl x)
    | Ast.StorageTag(x) -> Printf.sprintf "StorageTag(%s)" (storage x)
    | Ast.IncFileTag(x) -> Printf.sprintf "IncFileTag(%s)" (inc_file x)
    | Ast.Rule_elemTag(x) -> Printf.sprintf "Rule_elemTag(%s)" (rule_elem x)
    | Ast.StatementTag(x) -> Printf.sprintf "StatementTag(%s)" (statement x)
    | Ast.ForInfoTag(x) -> Printf.sprintf "ForInfoTag(%s)" (forinfo x)
    | Ast.CaseLineTag(x) -> Printf.sprintf "CaseLineTag(%s)" (case_line x)
    | Ast.StringFragmentTag(x) -> Printf.sprintf "StringFragmentTag(%s)" (string_fragment x)
    | Ast.AttributeTag(x) -> Printf.sprintf "AttributeTag(%s)" (print_attribute x)
    | Ast.AttrArgTag(x) -> Printf.sprintf "AttrArgTag(%s)" (print_attr_arg x)
    | Ast.ConstVolTag(x) -> Printf.sprintf "ConstVolTag(%s)" (const_vol x)
    | Ast.Token(x,Some info) -> Printf.sprintf "Token(%s,Some(%s))" (x) (str_info info)
    | Ast.Token(x,None) -> Printf.sprintf "Token(%s,None)" x
    | Ast.Directive(xs) ->
      let print = function
          Ast.Noindent s -> Printf.sprintf "Noindent(%s)" s
        | Ast.Indent s -> Printf.sprintf "Indent(%s)" s
        | Ast.Space s -> Printf.sprintf "Space(%s)" s in
      Printf.sprintf "Directive(%s)" (str_list print xs)
    | Ast.Code(x) -> Printf.sprintf "Code(%s)" (top_level x)
    | Ast.ExprDotsTag(x) -> Printf.sprintf "ExprDotsTag(%s)" (dots expression x)
    | Ast.ParamDotsTag(x) -> Printf.sprintf "ParamDotsTag(%s)" (parameter_list x)
    | Ast.TemplateParamDotsTag(x) -> Printf.sprintf "TemplateParamDotsTag(%s)" (dots templateParameterTypeDef x)
    | Ast.StmtDotsTag(x) -> Printf.sprintf "StmtDotsTag(%s)" (dots statement x)
    | Ast.AnnDeclDotsTag(x) -> Printf.sprintf "AnnDeclDotsTag(%s)" (dots annotated_decl x)
    | Ast.AnnFieldDotsTag(x) -> Printf.sprintf "AnnFieldDotsTag(%s)" (dots annotated_field x)
    | Ast.EnumDeclDotsTag(x) -> Printf.sprintf "EnumDeclDotsTag(%s)" (dots enum_decl x)
    | Ast.DefParDotsTag(x) -> Printf.sprintf "DefParDotsTag(%s)" (dots print_define_param x)
    | Ast.TypeCTag(x) -> Printf.sprintf "TypeCTag(%s)" (typeC x)
    | Ast.ParamTag(x) -> Printf.sprintf "ParamTag(%s)" (parameterTypeDef x)
    | Ast.TemplateParamTag(x) -> Printf.sprintf "TemplateParamTag(%s)" (templateParameterTypeDef x)
    | Ast.SgrepStartTag(x) -> Printf.sprintf "SgrepStartTag(%s)" x
    | Ast.SgrepEndTag(x) -> Printf.sprintf "SgrepEndTag(%s)" x

let rec dep = function
    Ast.Dep(s) -> Printf.sprintf "Dep(%s)" s
  | Ast.AntiDep(s) -> Printf.sprintf "AntiDep(%s)" s
  | Ast.EverDep(s) -> Printf.sprintf "EverDep(%s)" s
  | Ast.NeverDep(s) -> Printf.sprintf "NeverDep(%s)" s
  | Ast.AndDep(s1,s2) -> Printf.sprintf "AndDep(%s,%s)" (dep s1) (dep s2)
  | Ast.OrDep(s1,s2) -> Printf.sprintf "OrDep(%s,%s)" (dep s1) (dep s2)
  | Ast.FileIn s -> Printf.sprintf "FileIn(%s)" s
  | Ast.NotFileIn s -> Printf.sprintf "NotFileIn(%s)" s

let dependency = function
    Ast.NoDep  -> "NoDep"
  | Ast.FailDep -> "FailDep"
  | Ast.ExistsDep d -> Printf.sprintf "ExistsDep(%s)" (dep d)
  | Ast.ForallDep d -> Printf.sprintf "ForallDep(%s)" (dep d)

let str_mv mv = 
    str_list
    (function (script_name,inh_name,_ty,init) ->
      Printf.sprintf "(%s,%s,_,%s)"
      (match script_name with
	      (None,None) -> "(_,_)"
      |	(Some x,None) -> Printf.sprintf "(Some(%s),_)" x
      |	(None,Some a) -> Printf.sprintf "(_,Some(%s))" a
      |	(Some x,Some a) -> Printf.sprintf "(Some(%s),Some(%s))" x a)
      (Printf.sprintf "%s.%s" (fst inh_name) (snd inh_name))
      (match init with
        Ast.NoMVInit -> "NoMVInit"
      | Ast.MVInitString s -> Printf.sprintf "MVInitString(%s)" s
      | Ast.MVInitPosList -> "MVInitPosList")
    )
    mv

let unparse mvs z =
  Printf.printf "%s\n\n" (str_list (fun mv -> unparse_cocci_mv mv) mvs);
  match z with
    Ast.InitialScriptRule (name,lang,deps,mv,_pos,code) ->
      Printf.printf "InitialScriptRule(%s,%s,%s,%s,%s,%s)"
        name
        lang
        (dependency deps)
        (str_mv mv)
        "_"
        code
  | Ast.FinalScriptRule (name,lang,deps,mv,_pos,code) ->
      Printf.printf "FinalScriptRule(%s,%s,%s,%s,%s,%s)"
        name
        lang
        (dependency deps)
        (str_mv mv)
        "_"
        code
  | Ast.ScriptRule (name,lang,deps,bindings,script_vars,_pos,code) ->
      Printf.printf "ScriptRule(%s,%s,%s,%s,%s,%s,%s)"
        name
        lang
        (dependency deps)
        (str_mv bindings)
        "_"
        "_"
        code
  | Ast.CocciRule (nm, (deps, drops, exists), x, _, _) ->
      Printf.printf "CocciRule(%s, (%s, %s, %s), %s, _, _)"
        (Printf.sprintf "\"%s\"" nm)
        (match deps with
            Ast.NoDep -> "NoDep"
          | _ -> dependency deps)
        (match drops with
            [] -> "[]"
          |	_ -> (String.concat "," drops))
        (match exists with
            Ast.Exists -> "Exists"
          |	Ast.Forall -> " Forall"
          |	Ast.Undetermined -> "Undetermined")
        (rule x);
      Printf.printf "\n"