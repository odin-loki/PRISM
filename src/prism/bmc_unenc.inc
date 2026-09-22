static std::optional<std::string> unencoded_syntax_reason(const FunctionInfo& fn, std::string_view engine) {
    const std::string& body = fn.body;
    if (body.empty()) return std::nullopt;
    std::string blob = fn.signature + "\n" + body;
    // generated 575 gates from prism/bmc.py
    if (prism::re_search("\\{\\s*(?:\\[[^\\]]+\\]|\\.[A-Za-z_]\\w*)\\s*=", body)) {
        return std::format("designated init unencoded: {} is not a designated-init model", engine);
    }
    if (prism::re_search("\\bgoto\\s+\\*", body)) {
        return std::format("computed goto unencoded: {} is not a computed-goto model", engine);
    }
    if (prism::re_search("(?:^|[;{(]|=\\s*)\\&\\&[A-Za-z_]\\w*", body)) {
        return std::format("label-address unencoded: {} is not a label-address model", engine);
    }
    if (prism::re_search("\\b(?:_Thread_local|thread_local)\\b", body)) {
        return std::format("thread-local unencoded: {} is not a TLS model", engine);
    }
    if (prism::re_search("\\b(?:_Complex|_Imaginary)\\b", body)) {
        return std::format("complex unencoded: {} is not a complex arithmetic model", engine);
    }
    if (prism::re_search("\\b(?:typeof_unqual|__typeof_unqual__)\\s*\\(", body)) {
        return std::format("typeof_unqual unencoded: {} is not a typeof model", engine);
    }
    if (prism::re_search("\\b(?:typeof|__typeof__)\\s*\\(", body)) {
        return std::format("typeof unencoded: {} is not a typeof model", engine);
    }
    if (prism::re_search("(?m)(?:^|[;{])\\s*(?:void|int|unsigned(?:\\s+int)?|long(?:\\s+int)?|short|char|float|double|_Bool|bool)\\s+[A-Za-z_]\\w*\\s*\\([^)]*\\)\\s*\\{", body)) {
        return std::format("nested function unencoded: {} is not a nested-function model", engine);
    }
    if (prism::re_search("\\b(?:_Alignof|alignof)\\s*\\(", body)) {
        return std::format("alignof unencoded: {} is not an alignment model", engine);
    }
    if (prism::re_search("\\bva_arg\\s*\\(", body)) {
        return std::format("va_arg unencoded: {} is not a variadic model", engine);
    }
    if (prism::re_search("\\bfor\\s*\\([^)]*:[^)]*\\)", body)) {
        return std::format("C++ range-for unencoded: {} is not a range-for model", engine);
    }
    if (prism::re_search("\\[\\s*[^\\]]*\\]\\s*(?:\\([^)]*\\))?\\s*\\{", body)) {
        return std::format("C++ lambda unencoded: {} is not a lambda model", engine);
    }
    if (prism::re_search("\\bconst_cast\\s*<", body)) {
        return std::format("C++ const_cast unencoded: {} is not a cv-qualifier model", engine);
    }
    if (prism::re_search("\\bdynamic_cast\\s*<", body)) {
        return std::format("C++ dynamic_cast unencoded: {} is not an RTTI model", engine);
    }
    if (prism::re_search("\\btype_identity\\b", body)) {
        return std::format("C++ type_identity unencoded: {} is not a type_identity model", engine);
    }
    if (prism::re_search("\\btypeid\\s*\\(", body)) {
        return std::format("C++ typeid unencoded: {} is not an RTTI model", engine);
    }
    if (prism::re_search("\\breinterpret_cast\\s*<", body)) {
        return std::format("C++ reinterpret_cast unencoded: {} is not a type-pun model", engine);
    }
    if (prism::re_search("\\bstd::bit_cast\\b|bit_cast\\s*<", body)) {
        return std::format("bit_cast unencoded: {} is not a type-pun model", engine);
    }
    if (prism::re_search("(?m)\\bstd\\s*::\\s*jthread\\b|(?:^|[;{])\\s*jthread\\s+[A-Za-z_]\\w*\\s*\\(", body)) {
        return std::format("C++ std::jthread unencoded: {} is not a jthread model", engine);
    }
    if (prism::re_search("(?m)\\bstd\\s*::\\s*thread\\b|(?:^|[;{])\\s*thread\\s+[A-Za-z_]\\w*\\s*\\(", body)) {
        return std::format("C++ std::thread unencoded: {} is not a thread-lifetime model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*(?:async|future|promise)\\b|(?:future|promise)\\s*<", body)) {
        return std::format("C++ std::async unencoded: {} is not a future model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?packaged_task\\b|\\bpackaged_task\\s*<", body)) {
        return std::format("C++ packaged_task unencoded: {} is not a packaged_task model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?counted_iterator\\b", body)) {
        return std::format("C++ counted_iterator unencoded: {} is not a counted_iterator model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?weak_ptr\\s*<", body)) {
        return std::format("C++ weak_ptr unencoded: {} is not a weak_ptr model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?nested_exception\\b", body)) {
        return std::format("C++ nested_exception unencoded: {} is not a nested_exception model", engine);
    }
    if (prism::re_search("\\b(?:throw_with_nested|rethrow_if_nested)\\s*\\(", body)) {
        return std::format("throw_with_nested unencoded: unconstrained throw_with_nested is not a proof ({})", engine);
    }
    if (prism::re_search("\\buncaught_exceptions\\s*\\(", body)) {
        return std::format("C++ uncaught_exceptions unencoded: {} is not an uncaught_exceptions model", engine);
    }
    if (prism::re_search("\\bcurrent_exception\\s*\\(", body)) {
        return std::format("C++ current_exception unencoded: {} is not a current_exception model", engine);
    }
    if (prism::re_search("\\bmake_exception_ptr\\s*\\(", body)) {
        return std::format("C++ make_exception_ptr unencoded: {} is not a make_exception_ptr model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?exception_ptr\\b", body)) {
        return std::format("C++ exception_ptr unencoded: {} is not an exception_ptr model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?coroutine_handle\\b", body)) {
        return std::format("C++ coroutine_handle unencoded: {} is not a coroutine_handle model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?valarray\\s*<", body)) {
        return std::format("C++ valarray unencoded: {} is not a valarray model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?to_underlying\\s*\\(", body)) {
        return std::format("C++ to_underlying unencoded: {} is not a to_underlying model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?unexpected\\s*<", body)) {
        return std::format("C++ unexpected unencoded: {} is not an unexpected model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?function_ref\\s*<", body)) {
        return std::format("C++ function_ref unencoded: {} is not a function_ref model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*(?:move_only_function|copyable_function)\\b|\\b(?:move_only_function|copyable_function)\\s*<", body)) {
        return std::format("C++ move_only_function unencoded: {} is not a move_only_function model", engine);
    }
    if (prism::re_search("\\breference_wrapper\\b|\\bstd\\s*::\\s*(?:cref|ref)\\s*\\(", body)) {
        return std::format("C++ reference_wrapper unencoded: {} is not a reference_wrapper model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*function\\b|function\\s*<", body)) {
        return std::format("C++ std::function unencoded: {} is not a type-erased callable model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*mdspan\\b|mdspan\\s*<", body)) {
        return std::format("C++ std::mdspan unencoded: {} is not an mdspan model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*(?:mutex|lock_guard|unique_lock|scoped_lock)\\b|(?:lock_guard|unique_lock|scoped_lock)\\s*<", body)) {
        return std::format("C++ std::mutex unencoded: {} is not a C++ mutex model", engine);
    }
    if (prism::re_search("\\bnotify_all_at_thread_exit\\s*\\(", body)) {
        return std::format("C++ notify_all_at_thread_exit unencoded: {} is not a notify_all_at_thread_exit model", engine);
    }
    if (prism::re_search("\\bcondition_variable_any\\b", body)) {
        return std::format("C++ condition_variable_any unencoded: {} is not a condition_variable_any model", engine);
    }
    if (prism::re_search("\\bshared_timed_mutex\\b", body)) {
        return std::format("C++ shared_timed_mutex unencoded: {} is not a shared_timed_mutex model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*(?:condition_variable|shared_mutex)\\b|\\b(?:condition_variable|shared_mutex)\\b", body)) {
        return std::format("C++ std::condition_variable unencoded: {} is not a condvar/shared-mutex model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*atomic_ref\\b|atomic_ref\\s*<", body)) {
        return std::format("C++ std::atomic_ref unencoded: {} is not an atomic_ref model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*generator\\b|generator\\s*<", body)) {
        return std::format("C++ std::generator unencoded: {} is not a generator model", engine);
    }
    if (prism::re_search("\\[\\[\\s*assume\\s*\\(", body)) {
        return std::format("C++ assume unencoded: {} is not an assume-attribute model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*bind\\s*\\(", body)) {
        return std::format("C++ std::bind unencoded: {} is not a bind model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*any\\b|\\bany_cast\\s*<|\\bany_cast\\b", body)) {
        return std::format("C++ std::any unencoded: {} is not an any model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*filesystem\\b|\\bstd\\s*::\\s*fs\\s*::|\\bfilesystem\\s*::", body)) {
        return std::format("C++ std::filesystem unencoded: {} is not a filesystem model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*regex\\b|\\bstd\\s*::\\s*regex_|\\bregex\\s+[A-Za-z_]\\w*\\s*[\\({]", body)) {
        return std::format("C++ std::regex unencoded: {} is not a regex model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*(?:latch|barrier|counting_semaphore)\\b|\\blatch\\s+[A-Za-z_]\\w*\\s*\\(|\\bbarrier\\s*<", body)) {
        return std::format("C++ std::latch/barrier unencoded: {} is not a sync primitive model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*from_chars\\b|\\bfrom_chars\\s*\\(", body)) {
        return std::format("C++ from_chars unencoded: {} is not a charconv model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?to_chars\\s*\\(", body)) {
        return std::format("C++ to_chars unencoded: {} is not a charconv model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?hazard_pointer\\b", body)) {
        return std::format("C++ hazard_pointer unencoded: {} is not a hazard_pointer model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?text_encoding\\b", body)) {
        return std::format("C++ text_encoding unencoded: {} is not a text_encoding model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*(?:experimental\\s*::\\s*)?)?simd\\s*<", body)) {
        return std::format("C++ simd unencoded: {} is not a simd model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*visit\\b", body)) {
        return std::format("C++ std::visit unencoded: {} is not a visitor model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?source_location\\b", body)) {
        return std::format("C++ source_location unencoded: {} is not a source_location model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?stacktrace\\b", body)) {
        return std::format("C++ stacktrace unencoded: {} is not a stacktrace model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*stop_(?:token|source|callback)\\b", body)) {
        return std::format("C++ stop_token unencoded: {} is not a stop_token model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*flat_map\\b|\\bflat_map\\s*<", body)) {
        return std::format("C++ flat_map unencoded: {} is not a flat_map model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*flat_set\\b|\\bflat_set\\s*<", body)) {
        return std::format("C++ flat_set unencoded: {} is not a flat_set model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*flat_multiset\\b|\\bflat_multiset\\s*<", body)) {
        return std::format("C++ flat_multiset unencoded: {} is not a flat_multiset model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*flat_multimap\\b|\\bflat_multimap\\s*<", body)) {
        return std::format("C++ flat_multimap unencoded: {} is not a flat_multimap model", engine);
    }
    if (prism::re_search("\\bzoned_time\\b", body)) {
        return std::format("C++ zoned_time unencoded: {} is not a zoned_time model", engine);
    }
    if (prism::re_search("\\b(?:current_zone|tzdb)\\b", body)) {
        return std::format("C++ tzdb unencoded: {} is not a tzdb model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*chrono\\b|\\bchrono\\s*::", body)) {
        return std::format("C++ chrono unencoded: {} is not a chrono model", engine);
    }
    if (prism::re_search("\\bzip_transform(?:_view)?\\b", body)) {
        return std::format("C++ zip_transform unencoded: {} is not a zip_transform model", engine);
    }
    if (prism::re_search("views\\s*::\\s*zip|\\bzip_view\\b", body)) {
        return std::format("C++ views::zip unencoded: {} is not a views::zip model", engine);
    }
    if (prism::re_search("\\bas_rvalue(?:_view)?\\b", body)) {
        return std::format("C++ as_rvalue unencoded: {} is not an as_rvalue model", engine);
    }
    if (prism::re_search("\\bis_scoped_enum\\b", body)) {
        return std::format("C++ is_scoped_enum unencoded: {} is not an is_scoped_enum model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*)?enumerate(?:_view)?\\b", body)) {
        return std::format("C++ views::enumerate unencoded: {} is not a views::enumerate model", engine);
    }
    if (prism::re_search("\\bcartesian_product(?:_view)?\\b", body)) {
        return std::format("C++ cartesian_product unencoded: {} is not a cartesian_product model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*chunk(?:_by)?|chunk(?:_by|_view))\\b", body)) {
        return std::format("C++ views::chunk unencoded: {} is not a views::chunk model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*slide|slide_view)\\b", body)) {
        return std::format("C++ views::slide unencoded: {} is not a views::slide model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*adjacent(?:_transform)?|adjacent(?:_transform|_view))\\b", body)) {
        return std::format("C++ views::adjacent unencoded: {} is not a views::adjacent model", engine);
    }
    if (prism::re_search("\\bjoin_with(?:_view)?\\b", body)) {
        return std::format("C++ join_with unencoded: {} is not a join_with model", engine);
    }
    if (prism::re_search("views\\s*::\\s*join|\\bjoin_view\\b", body)) {
        return std::format("C++ views::join unencoded: {} is not a views::join model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*)?stride(?:_view)?\\b", body)) {
        return std::format("C++ views::stride unencoded: {} is not a views::stride model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*repeat|repeat_view)\\b", body)) {
        return std::format("C++ views::repeat unencoded: {} is not a views::repeat model", engine);
    }
    if (prism::re_search("\\btake_while(?:_view)?\\b", body)) {
        return std::format("C++ views::take_while unencoded: {} is not a take_while model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*take\\b|\\btake_view\\b)", body)) {
        return std::format("C++ views::take unencoded: {} is not a views::take model", engine);
    }
    if (prism::re_search("\\bdrop_while(?:_view)?\\b", body)) {
        return std::format("C++ views::drop_while unencoded: {} is not a drop_while model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*drop\\b|\\bdrop_view\\b)", body)) {
        return std::format("C++ views::drop unencoded: {} is not a views::drop model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*keys\\b|\\bkeys_view\\b)", body)) {
        return std::format("C++ views::keys unencoded: {} is not a keys model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*values\\b|\\bvalues_view\\b)", body)) {
        return std::format("C++ views::values unencoded: {} is not a values model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*reverse\\b|\\breverse_view\\b)", body)) {
        return std::format("C++ views::reverse unencoded: {} is not a reverse_view model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*counted\\b|\\bcounted_view\\b)", body)) {
        return std::format("C++ views::counted unencoded: {} is not a counted_view model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*filter\\b|\\bfilter_view\\b)", body)) {
        return std::format("C++ views::filter unencoded: {} is not a views::filter model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*transform\\b|\\btransform_view\\b)", body)) {
        return std::format("C++ views::transform unencoded: {} is not a transform_view model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*elements\\b|\\belements_view\\b)", body)) {
        return std::format("C++ views::elements unencoded: {} is not an elements model", engine);
    }
    if (prism::re_search("\\b(?:views\\s*::\\s*iota\\b|\\biota_view\\b)", body)) {
        return std::format("C++ views::iota unencoded: {} is not an iota model", engine);
    }
    if (prism::re_search("\\bfrom_range\\b", body)) {
        return std::format("C++ from_range unencoded: {} is not a from_range model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*ranges\\s*::\\s*views\\b|\\bstd\\s*::\\s*views\\s*::", body)) {
        return std::format("C++ ranges views unencoded: {} is not a ranges-views model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*hive\\b|\\bhive\\s*<", body)) {
        return std::format("C++ hive unencoded: {} is not a hive model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*(?:execution\\s*::\\s*)?task\\s*<", body)) {
        return std::format("C++ std::task unencoded: {} is not a task model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*execution\\s*::", body)) {
        return std::format("C++ execution unencoded: {} is not an execution model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*indirect\\b|\\bindirect\\s*<|\\bstd\\s*::\\s*polymorphic\\b|\\bpolymorphic\\s*<", body)) {
        return std::format("C++ indirect unencoded: {} is not an indirect/polymorphic model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*bitset\\s*<|\\bbitset\\s*<", body)) {
        return std::format("C++ bitset unencoded: {} is not a bitset model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?(?:basic_)?(?:string|ostring|istring)stream\\b", body)) {
        return std::format("C++ stringstream unencoded: {} is not a stringstream model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?(?:basic_)?osyncstream\\b", body)) {
        return std::format("C++ osyncstream unencoded: {} is not an osyncstream model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?(?:basic_)?syncbuf\\b", body)) {
        return std::format("C++ syncbuf unencoded: {} is not a syncbuf model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?(?:basic_)?(?:i|o)?spanstream\\b", body)) {
        return std::format("C++ spanstream unencoded: {} is not a spanstream model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?(?:inout_ptr|out_ptr)\\s*(?:<|\\()", body)) {
        return std::format("C++ out_ptr unencoded: {} is not an out_ptr model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*rcu\\b|\\brcu_synchronize\\s*\\(|\\brcu_obj\\s*<", body)) {
        return std::format("C++ rcu unencoded: {} is not an rcu model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*linalg\\b|\\blinalg\\s*::", body)) {
        return std::format("C++ linalg unencoded: {} is not a linalg model", engine);
    }
    if (prism::re_search("\\bsync_wait\\s*\\(", body)) {
        return std::format("C++ sync_wait unencoded: {} is not an execution model", engine);
    }
    if (prism::re_search("\\bcontract_assert\\s*\\(|\\[\\[\\s*(?:pre|post)\\s*:", body)) {
        return std::format("C++ contracts unencoded: {} is not a contracts model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*meta\\b|\\^\\^|\\bdefine_aggregate\\s*\\(|\\bdefine_class\\s*\\(", body)) {
        return std::format("C++ reflection unencoded: {} is not a reflection model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?initializer_list\\s*<", body)) {
        return std::format("C++ initializer_list unencoded: {} is not a temporary-lifetime model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?optional\\s*<", body)) {
        return std::format("C++ std::optional unencoded: {} is not an optional model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*expected\\b|\\bexpected\\s*<", body)) {
        return std::format("C++ std::expected unencoded: {} is not an expected model", engine);
    }
    if (prism::re_search("\\bformat_to(?:_n)?\\s*\\(", body)) {
        return std::format("C++ format_to unencoded: {} is not a format_to model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*(?:format|print|println)\\b", body)) {
        return std::format("C++ std::format unencoded: {} is not a format model", engine);
    }
    if (prism::re_search("<=>", body)) {
        return std::format("C++ spaceship unencoded: {} is not a three-way comparison model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?variant\\s*<", body)) {
        return std::format("C++ std::variant unencoded: {} is not a variant model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?span\\s*<", body)) {
        return std::format("C++ std::span unencoded: {} is not a span-lifetime model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*inplace_vector\\b|\\binplace_vector\\s*<", body)) {
        return std::format("C++ inplace_vector unencoded: {} is not an inplace_vector model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?vector\\s*<", body)) {
        return std::format("C++ std::vector unencoded: {} is not a container model", engine);
    }
    if (prism::re_search("\\bcatch\\s*\\(\\s*\\.\\.\\.\\s*\\)", body)) {
        return std::format("C++ catch-all unencoded: {} is not an exception model", engine);
    }
    if (prism::re_search("\\bthrow\\s+new\\b", body)) {
        return std::format("C++ throw-new unencoded: {} is not an exception model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*launder\\b|\\blaunder\\s*\\(", body)) {
        return std::format("C++ launder unencoded: {} is not a lifetime model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?start_lifetime_as(?:_array)?\\b", body)) {
        return std::format("C++ start_lifetime_as unencoded: {} is not a lifetime model", engine);
    }
    if (prism::re_search("\\b(?:enable_shared_from_this|shared_from_this)\\b", body)) {
        return std::format("C++ shared_from_this unencoded: {} is not a shared-lifetime model", engine);
    }
    if (prism::re_search("\\bif\\s+constexpr\\b", body)) {
        return std::format("if constexpr unencoded: {} is not a compile-time-if model", engine);
    }
    if (prism::re_search("\\bconstexpr\\b", body)) {
        return std::format("constexpr unencoded: {} is not a constexpr model", engine);
    }
    if (prism::re_search("(?:\\brequires\\s*\\()|(?:\\bconcept\\s+)", blob)) {
        return std::format("C++ concepts unencoded: {} is not a concepts model", engine);
    }
    if (prism::re_search("\\bcase\\s+[^:'\\\"]+?\\s*\\.\\.\\.\\s*[^:'\\\"]+?:", body)) {
        return std::format("case-range unencoded: {} is not a case-range model", engine);
    }
    if (prism::re_search("\\(\\s*\\.\\.\\.\\s*[+\\-|&^]|[+\\-|&^]\\s*\\.\\.\\.\\s*\\)", body)) {
        return std::format("C++ fold unencoded: {} is not a fold-expression model", engine);
    }
    if (prism::re_search("\\b(?:__int128(?:_t)?|_BitInt)\\b", body)) {
        return std::format("128-bit unencoded: {} is not a 128-bit model", engine);
    }
    if (prism::re_search("\\b(?:_Decimal32|_Decimal64|_Decimal128)\\b", body)) {
        return std::format("decimal float unencoded: {} is not a decimal-float model", engine);
    }
    if (prism::re_search("\\b(?:_Float16|_Float32|_Float64|__fp16)\\b", body)) {
        return std::format("extra-IEEE float unencoded: {} is not an extra-IEEE model", engine);
    }
    // Python engine prism/bmc.py: re.sub(r"/\*.*?\*/", " ", flags=re.S) then
    // re.sub(r"//.*?$", " ", flags=re.M) before nullptr / pragma pack / import.
    auto unenc_strip_comments = [](const std::string& src) {
        auto sub = [](const std::string& s, const char* pat, bool multiline, bool dotall) {
            prism::Regex re(pat, multiline, dotall);
            std::string out;
            std::size_t off = 0;
            for (auto& m : re.finditer(s)) {
                auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
                auto b = static_cast<std::size_t>(std::max(0, m.spans[0].second));
                if (a < off) continue;
                out.append(s, off, a - off);
                out.push_back(' ');
                off = b;
            }
            out.append(s, off, std::string::npos);
            return out;
        };
        auto t = sub(src, R"(/\*.*?\*/)", false, true);
        return sub(t, R"(//.*?$)", true, false);
    };
    {
        auto code = unenc_strip_comments(body);
        if (prism::re_search("\\bnullptr\\b", code)) {
            return std::format("C23 nullptr unencoded: {} is not a nullptr model", engine);
        }
    }
    if (prism::re_search("\\bco_(?:await|yield|return)\\b", body)) {
        return std::format("C++ coroutine unencoded: {} is not a coroutine model", engine);
    }
    if (prism::re_search("__attribute__\\s*\\(\\s*\\(\\s*packed\\s*\\)\\s*\\)|\\b__packed\\b", body)) {
        return std::format("packed layout unencoded: {} is not a packed-layout model", engine);
    }
    {
        // Python engine prism/bmc.py: file + body, comments stripped, then pragma pack.
        std::string pack_src = body;
        if (!fn.file.empty()) {
            try {
                std::ifstream in(fn.file, std::ios::binary);
                if (in) {
                    in.seekg(0, std::ios::end);
                    auto sz = in.tellg();
                    if (sz > 0 && sz < 1000000) {
                        in.seekg(0);
                        std::ostringstream ss;
                        ss << in.rdbuf();
                        pack_src = ss.str() + "\n" + body;
                    }
                }
            } catch (...) {
                pack_src = body;
            }
        }
        auto pack_code = unenc_strip_comments(pack_src);
        if (prism::re_search("\\bpragma\\s+pack\\b", pack_code)) {
            return std::format("pragma pack unencoded: {} is not a packed-layout model", engine);
        }
    }
    {
        auto import_code = unenc_strip_comments(body);
        if (prism::re_search("(?:\\bimport\\s+[A-Za-z_])|(?:\\bexport\\s+module\\b)", import_code)) {
            return std::format("C++ module import unencoded: {} is not a modules model", engine);
        }
    }
    if (prism::re_search("\\#\\s*embed\\b", body)) {
        return std::format("C++ embed unencoded: {} is not an embed model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*rcu\\b|\\brcu_synchronize\\s*\\(|\\brcu_obj\\s*<", body)) {
        return std::format("C++ rcu unencoded: {} is not an rcu model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*linalg\\b|\\blinalg\\s*::", body)) {
        return std::format("C++ linalg unencoded: {} is not a linalg model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*meta\\b|\\^\\^", body)) {
        return std::format("C++ reflection unencoded: {} is not a reflection model", engine);
    }
    if (prism::re_search("__attribute__\\s*\\(\\s*\\(\\s*cleanup", body)) {
        return std::format("cleanup attribute unencoded: {} is not a cleanup model", engine);
    }
    if (prism::re_search("__attribute__\\s*\\(\\s*\\(\\s*(?:__)?vector_size|\\b__vector_size\\b", body)) {
        return std::format("vector_size unencoded: {} is not a SIMD vector model", engine);
    }
    if (prism::re_search("\\bL'(?:\\\\.|[^\\\\'])'", body)) {
        return std::format("wide character unencoded: {} is not a wide-char model", engine);
    }
    if (prism::re_search("\\bL\"(?:\\\\.|[^\\\\\"])*\"", body)) {
        return std::format("wide character unencoded: {} is not a wide-char model", engine);
    }
    if (prism::re_search("\\bexecveat\\s*\\(", body)) {
        return std::format("execveat unencoded: unconstrained execveat is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpthread_atfork\\s*\\(", body)) {
        return std::format("pthread_atfork unencoded: unconstrained pthread_atfork is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpledge\\s*\\(", body)) {
        return std::format("pledge unencoded: unconstrained pledge is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmac_(?:set|get)_(?:proc|fd|file)\\s*\\(", body)) {
        return std::format("mac_set_proc unencoded: unconstrained mac_set_proc is not a proof ({})", engine);
    }
    if (prism::re_search("\\bcap_getmode\\s*\\(", body)) {
        return std::format("cap_getmode unencoded: unconstrained cap_getmode is not a proof ({})", engine);
    }
    if (prism::re_search("\\bcap_getrights\\s*\\(", body)) {
        return std::format("cap_getrights unencoded: unconstrained cap_getrights is not a proof ({})", engine);
    }
    if (prism::re_search("\\bcap_enter\\s*\\(", body)) {
        return std::format("cap_enter unencoded: unconstrained cap_enter is not a proof ({})", engine);
    }
    if (prism::re_search("\\bcap_sandboxed\\s*\\(", body)) {
        return std::format("cap_sandboxed unencoded: unconstrained cap_sandboxed is not a proof ({})", engine);
    }
    if (prism::re_search("\\bcap_rights_(?:limit|get)\\s*\\(", body)) {
        return std::format("cap_rights unencoded: unconstrained cap_rights_limit is not a proof ({})", engine);
    }
    if (prism::re_search("\\bcap_(?:fcntls|ioctls)_limit\\s*\\(", body)) {
        return std::format("cap_fcntls unencoded: unconstrained cap_fcntls_limit is not a proof ({})", engine);
    }
    if (prism::re_search("\\bunveil\\s*\\(", body)) {
        return std::format("unveil unencoded: unconstrained unveil is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsysctl(?:byname)?\\s*\\(", body)) {
        return std::format("sysctl unencoded: unconstrained sysctl is not a proof ({})", engine);
    }
    if (prism::re_search("\\bkqueue\\s*\\(", body)) {
        return std::format("kqueue unencoded: unconstrained kqueue is not a proof ({})", engine);
    }
    if (prism::re_search("\\bkevent\\s*\\(", body)) {
        return std::format("kevent unencoded: unconstrained kevent is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpause\\s*\\(", body)) {
        return std::format("pause unencoded: unconstrained pause is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:get|set|swap|make)context\\s*\\(", body)) {
        return std::format("getcontext unencoded: unconstrained getcontext is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpthread_attr_(?:init|destroy|setstacksize|setstack|setdetachstate|getstacksize|getstack|getdetachstate)\\s*\\(", body)) {
        return std::format("pthread_attr unencoded: unconstrained pthread_attr_init is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:fork|vfork|execlp|execle|execl|execvpe|execvp|execve|execv)\\s*\\(", body)) {
        return std::format("process spawn unencoded: unconstrained fork/exec is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpdfork\\s*\\(", body)) {
        return std::format("pdfork unencoded: unconstrained pdfork is not a proof ({})", engine);
    }
    if (prism::re_search("\\brfork\\s*\\(", body)) {
        return std::format("rfork unencoded: unconstrained rfork is not a proof ({})", engine);
    }
    if (prism::re_search("\\bminherit\\s*\\(", body)) {
        return std::format("minherit unencoded: unconstrained minherit is not a proof ({})", engine);
    }
    if (prism::re_search("\\bnfssvc\\s*\\(", body)) {
        return std::format("nfssvc unencoded: unconstrained nfssvc is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsysarch\\s*\\(", body)) {
        return std::format("sysarch unencoded: unconstrained sysarch is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetbootfile\\s*\\(", body)) {
        return std::format("getbootfile unencoded: unconstrained getbootfile is not a proof ({})", engine);
    }
    if (prism::re_search("\\bdevname(?:_r)?\\s*\\(", body)) {
        return std::format("devname unencoded: unconstrained devname is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsbrk\\s*\\(", body)) {
        return std::format("sbrk unencoded: unconstrained sbrk is not a proof ({})", engine);
    }
    if (prism::re_search("\\bbrk\\s*\\(", body)) {
        return std::format("brk unencoded: unconstrained brk is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:mmap|munmap|mprotect)\\s*\\(", body)) {
        return std::format("mmap unencoded: {} is not a VM model", engine);
    }
    if (prism::re_search("\\bioctl\\s*\\(", body)) {
        return std::format("ioctl unencoded: unconstrained ioctl is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmod(?:find|stat|next|fnext)\\s*\\(", body)) {
        return std::format("modfind unencoded: unconstrained modfind is not a proof ({})", engine);
    }
    if (prism::re_search("\\bkld(?:firstmod|nextmod)\\s*\\(", body)) {
        return std::format("kldfirstmod unencoded: unconstrained kldfirstmod is not a proof ({})", engine);
    }
    if (prism::re_search("\\bkld_(?:isloaded|load)\\s*\\(", body)) {
        return std::format("kld_load unencoded: unconstrained kld_load is not a proof ({})", engine);
    }
    if (prism::re_search("\\bkld(?:load|unload|find|sym|stat)\\s*\\(", body)) {
        return std::format("kldload unencoded: unconstrained kldload is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:dlfunc|dlvsym)\\s*\\(", body)) {
        return std::format("dlfunc unencoded: unconstrained dlfunc is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:dlopen|dlsym|dlclose)\\s*\\(", body)) {
        return std::format("dlopen unencoded: {} is not a dynamic-loader model", engine);
    }
    if (prism::re_search("\\b(?:__builtin_clzll|__builtin_ctzll|__builtin_clz|__builtin_ctz)\\s*\\(", body)) {
        return std::format("clz unencoded: unconstrained __builtin_clz is UB on 0 and is not a proof ({})", engine);
    }
    if (prism::re_search("\\b__builtin_choose_expr\\s*\\(", body)) {
        return std::format("choose_expr unencoded: {} is not a __builtin_choose_expr model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*unreachable\\s*\\(", body)) {
        return std::format("C++ std::unreachable unencoded: {} is not an unreachable model", engine);
    }
    if (prism::re_search("\\b(?:accept4|accept)\\s*\\(", body)) {
        return std::format("accept unencoded: unconstrained accept is not a proof ({})", engine);
    }
    if (prism::re_search("\\bfchmodat2\\s*\\(", body)) {
        return std::format("fchmodat2 unencoded: unconstrained fchmodat2 is not a proof ({})", engine);
    }
    if (prism::re_search("\\bfchmodat\\s*\\(", body)) {
        return std::format("fchmodat unencoded: unconstrained fchmodat is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:fchmod|chmod)\\s*\\(", body)) {
        return std::format("chmod unencoded: unconstrained chmod is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:f|l)?chflags\\s*\\(", body)) {
        return std::format("chflags unencoded: unconstrained chflags is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:setreuid|setregid|setresuid|setresgid)\\s*\\(", body)) {
        return std::format("setreuid unencoded: unconstrained setreuid is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetres(?:uid|gid)\\s*\\(", body)) {
        return std::format("getresuid unencoded: unconstrained getresuid is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:setfsuid|setfsgid)\\s*\\(", body)) {
        return std::format("setfsuid unencoded: unconstrained setfsuid is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:setpgid|setsid|getsid)\\s*\\(", body)) {
        return std::format("setpgid unencoded: unconstrained setpgid is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:setuid|seteuid|setgid)\\s*\\(", body)) {
        return std::format("setuid unencoded: unconstrained setuid is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsocket\\s*\\(", body)) {
        return std::format("socket unencoded: unconstrained socket is not a proof ({})", engine);
    }
    if (prism::re_search("(?<![:\\w])bind\\s*\\(", body)) {
        return std::format("bind unencoded: unconstrained bind is not a proof ({})", engine);
    }
    if (prism::re_search("\\blisten\\s*\\(", body)) {
        return std::format("listen unencoded: unconstrained listen is not a proof ({})", engine);
    }
    if (prism::re_search("\\bconnect\\s*\\(", body)) {
        return std::format("connect unencoded: unconstrained connect is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpipe2?\\s*\\(", body)) {
        return std::format("pipe unencoded: {} is not a pipe model", engine);
    }
    if (prism::re_search("\\bdup[23]?\\s*\\(", body)) {
        return std::format("dup unencoded: {} is not an fd model", engine);
    }
    if (prism::re_search("\\bfcntl\\s*\\(", body)) {
        return std::format("fcntl unencoded: unconstrained fcntl is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpd(?:getpid|wait4)\\s*\\(", body)) {
        return std::format("pdgetpid unencoded: unconstrained pdgetpid is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:wait4|wait3)\\s*\\(", body)) {
        return std::format("wait4 unencoded: unconstrained wait4 is not a proof ({})", engine);
    }
    if (prism::re_search("\\bwait6\\s*\\(", body)) {
        return std::format("wait6 unencoded: unconstrained wait6 is not a proof ({})", engine);
    }
    if (prism::re_search("\\bwait(?:pid|id)?\\s*\\(", body)) {
        return std::format("wait unencoded: unconstrained wait is not a proof ({})", engine);
    }
    if (prism::re_search("\\bunlinkat\\s*\\(", body)) {
        return std::format("unlinkat unencoded: unconstrained unlinkat is not a proof ({})", engine);
    }
    if (prism::re_search("\\bunlink\\s*\\(", body)) {
        return std::format("unlink unencoded: unconstrained unlink is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmknodat\\s*\\(", body)) {
        return std::format("mknodat unencoded: unconstrained mknodat is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:mkfifo|mknod)\\s*\\(", body)) {
        return std::format("mkfifo unencoded: unconstrained mkfifo is not a proof ({})", engine);
    }
    if (prism::re_search("\\bepoll_create(?:1)?\\s*\\(", body)) {
        return std::format("epoll_create unencoded: unconstrained epoll_create is not a proof ({})", engine);
    }
    if (prism::re_search("\\bepoll_pwait(?:2)?\\s*\\(", body)) {
        return std::format("epoll_pwait unencoded: unconstrained epoll_pwait is not a proof ({})", engine);
    }
    if (prism::re_search("\\bppoll\\s*\\(", body)) {
        return std::format("ppoll unencoded: unconstrained ppoll is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:pselect|select|epoll_wait|epoll_ctl|poll)\\s*\\(", body)) {
        return std::format("select unencoded: unconstrained I/O multiplex is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:sendmsg|recvmsg)\\s*\\(", body)) {
        return std::format("sendmsg unencoded: unconstrained sendmsg is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:sendto|recvfrom|send|recv|shutdown)\\s*\\(", body)) {
        return std::format("send unencoded: unconstrained socket I/O is not a proof ({})", engine);
    }
    if (prism::re_search("\\brt_(?:tgsigqueueinfo|sigqueueinfo)\\s*\\(", body)) {
        return std::format("rt_sigqueueinfo unencoded: unconstrained rt_sigqueueinfo is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsigqueue\\s*\\(", body)) {
        return std::format("sigqueue unencoded: unconstrained sigqueue is not a proof ({})", engine);
    }
    if (prism::re_search("\\bkill_dependency\\s*\\(", body)) {
        return std::format("C++ kill_dependency unencoded: {} is not a kill_dependency model", engine);
    }
    if (prism::re_search("\\bthr_(?:new|kill2|kill|self|exit|suspend|wake)\\s*\\(", body)) {
        return std::format("thr_kill unencoded: unconstrained thr_kill is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpthread_kill\\s*\\(", body)) {
        return std::format("pthread_kill unencoded: unconstrained pthread_kill is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:kill|raise|alarm)\\s*\\(", body)) {
        return std::format("kill unencoded: unconstrained signal delivery is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:getaddrinfo|freeaddrinfo)\\s*\\(", body)) {
        return std::format("addrinfo unencoded: unconstrained DNS is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpthread_cancel\\s*\\(", body)) {
        return std::format("pthread_cancel unencoded: unconstrained pthread_cancel is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpthread_(?:key_create|key_delete|setspecific|getspecific)\\s*\\(", body)) {
        return std::format("pthread_key_create unencoded: unconstrained pthread_key_create is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:pthread_join|pthread_detach)\\s*\\(", body)) {
        return std::format("pthread_join unencoded: unconstrained pthread_join is not a proof ({})", engine);
    }
    if (prism::re_search("\\bthrd_(?:create|join|detach|exit|sleep|yield|current|equal)\\s*\\(", body)) {
        return std::format("ISO C11 thrd unencoded: unconstrained thrd_* is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:sem_wait|sem_post)\\s*\\(", body)) {
        return std::format("sem unencoded: unconstrained sem_wait is not a proof ({})", engine);
    }
    if (prism::re_search("\\bopenat\\s*\\(", body)) {
        return std::format("openat unencoded: unconstrained openat is not a proof ({})", engine);
    }
    if (prism::re_search("\\bflock\\s*\\(", body)) {
        return std::format("flock unencoded: unconstrained flock is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:fchown|lchown|chown)\\s*\\(", body)) {
        return std::format("chown unencoded: unconstrained chown is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsymlinkat\\s*\\(", body)) {
        return std::format("symlinkat unencoded: unconstrained symlinkat is not a proof ({})", engine);
    }
    if (prism::re_search("\\breadlinkat\\s*\\(", body)) {
        return std::format("readlinkat unencoded: unconstrained readlinkat is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:symlink|readlink)\\s*\\(", body)) {
        return std::format("symlink unencoded: unconstrained symlink is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:pthread_join|pthread_detach|pthread_once)\\s*\\(", body)) {
        return std::format("pthread join unencoded: unconstrained thread join is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:sem_wait|sem_post|sem_init|sem_destroy)\\s*\\(", body)) {
        return std::format("sem unencoded: {} is not a semaphore model", engine);
    }
    if (prism::re_search("\\bksem_(?:open|close|unlink|wait|post)\\s*\\(", body)) {
        return std::format("ksem_open unencoded: unconstrained ksem_open is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsem_(?:open|close|unlink)\\s*\\(", body)) {
        return std::format("sem_open unencoded: unconstrained sem_open is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsem_timedwait\\s*\\(", body)) {
        return std::format("sem_timedwait unencoded: unconstrained sem_timedwait is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsem_(?:trywait|getvalue)\\s*\\(", body)) {
        return std::format("sem_trywait unencoded: unconstrained sem_trywait is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpthread_spin_(?:try)?(?:lock|unlock|init|destroy)\\s*\\(", body)) {
        return std::format("pthread_spin unencoded: unconstrained pthread_spin_lock is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpthread_rwlock_(?:try|timed)?(?:rdlock|wrlock|unlock|init|destroy)\\s*\\(", body)) {
        return std::format("pthread_rwlock unencoded: unconstrained pthread_rwlock is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpthread_cond_(?:timedwait|wait|signal|broadcast|init|destroy)\\s*\\(", body)) {
        return std::format("pthread_cond unencoded: unconstrained pthread_cond_wait is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpthread_barrier_(?:wait|init|destroy)\\s*\\(", body)) {
        return std::format("pthread_barrier unencoded: unconstrained pthread_barrier_wait is not a proof ({})", engine);
    }
    if (prism::re_search("\\bopenat\\s*\\(", body)) {
        return std::format("openat unencoded: unconstrained openat is not a proof ({})", engine);
    }
    if (prism::re_search("\\bflock\\s*\\(", body)) {
        return std::format("flock unencoded: unconstrained flock is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:posix_memalign|aligned_alloc)\\s*\\(", body)) {
        return std::format("aligned_alloc unencoded: {} is not an aligned-alloc model", engine);
    }
    if (prism::re_search("\\bvalloc\\s*\\(", body)) {
        return std::format("valloc unencoded: unconstrained valloc is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:__atomic_load|__atomic_store|__sync_fetch_and_add|__sync_bool_compare_and_swap)\\s*\\(", body)) {
        return std::format("atomic builtin unencoded: {} is not an atomic-builtin model", engine);
    }
    if (prism::re_search("\\b(?:fchown|lchown|chown)\\s*\\(", body)) {
        return std::format("chown unencoded: unconstrained chown is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:symlink|readlink)\\s*\\(", body)) {
        return std::format("symlink unencoded: unconstrained symlink/readlink is not a proof ({})", engine);
    }
    if (prism::re_search("\\bfts_(?:open|read|children|close|set)\\s*\\(", body)) {
        return std::format("fts_open unencoded: unconstrained fts_open is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:fdopendir|opendir|readdir|closedir)\\s*\\(", body)) {
        return std::format("opendir unencoded: {} is not a DIR* model", engine);
    }
    if (prism::re_search("\\b(?:setrlimit|getrlimit)\\s*\\(", body)) {
        return std::format("setrlimit unencoded: unconstrained setrlimit is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:getsockname|getpeername)\\s*\\(", body)) {
        return std::format("getsockname unencoded: unconstrained getsockname is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetpeereid\\s*\\(", body)) {
        return std::format("getpeereid unencoded: unconstrained getpeereid is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:getsockopt|setsockopt)\\s*\\(", body)) {
        return std::format("getsockopt unencoded: unconstrained socket opts is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:listmount|statmount)\\s*\\(", body)) {
        return std::format("listmount unencoded: unconstrained listmount/statmount is not a proof ({})", engine);
    }
    if (prism::re_search("\\bustat\\s*\\(", body)) {
        return std::format("ustat unencoded: unconstrained ustat is not a proof ({})", engine);
    }
    if (prism::re_search("\\bfile_(?:get|set)attr\\s*\\(", body)) {
        return std::format("file_getattr unencoded: unconstrained file_getattr is not a proof ({})", engine);
    }
    if (prism::re_search("\\bfh(?:linkat|link|readlink)\\s*\\(", body)) {
        return std::format("fhlink unencoded: unconstrained fhlink is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:getfh|fhopen|fhstatfs|fhstat|getfhat)\\s*\\(", body)) {
        return std::format("getfh unencoded: unconstrained getfh is not a proof ({})", engine);
    }
    if (prism::re_search("\\bfstatat\\s*\\(", body)) {
        return std::format("fstatat unencoded: unconstrained fstatat is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:lstat|fstat|stat)\\s*\\(", body)) {
        return std::format("stat unencoded: unconstrained stat is not a proof ({})", engine);
    }
    if (prism::re_search("\\brenameat2\\s*\\(", body)) {
        return std::format("renameat2 unencoded: unconstrained renameat2 is not a proof ({})", engine);
    }
    if (prism::re_search("\\brenameat\\s*\\(", body)) {
        return std::format("renameat unencoded: unconstrained renameat is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmkdirat\\s*\\(", body)) {
        return std::format("mkdirat unencoded: unconstrained mkdirat is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:mkdir|rmdir|rename)\\s*\\(", body)) {
        return std::format("mkdir unencoded: unconstrained mkdir is not a proof ({})", engine);
    }
    if (prism::re_search("\\bcrypt_(?:newhash|checkpass)\\s*\\(", body)) {
        return std::format("crypt_newhash unencoded: unconstrained crypt_newhash is not a proof ({})", engine);
    }
    if (prism::re_search("\\bkenv\\s*\\(", body)) {
        return std::format("kenv unencoded: unconstrained kenv is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:getpwuid|getpwnam|crypt)\\s*\\(", body)) {
        return std::format("getpwuid unencoded: unconstrained getpwuid is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:clock_settime|clock_adjtime|clock_nanosleep)\\s*\\(", body)) {
        return std::format("clock_settime unencoded: unconstrained clock_settime is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsettimeofday\\s*\\(", body)) {
        return std::format("settimeofday unencoded: unconstrained settimeofday is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpthread_getcpuclockid\\s*\\(", body)) {
        return std::format("pthread_getcpuclockid unencoded: unconstrained pthread_getcpuclockid is not a proof ({})", engine);
    }
    if (prism::re_search("\\bclock_getcpuclockid\\s*\\(", body)) {
        return std::format("clock_getcpuclockid unencoded: unconstrained clock_getcpuclockid is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:clock_gettime|gettimeofday)\\s*\\(", body)) {
        return std::format("clock_gettime unencoded: unconstrained clock_gettime is not a proof ({})", engine);
    }
    if (prism::re_search("\\bposix_typed_mem_(?:open|get_info)\\s*\\(", body)) {
        return std::format("posix_typed_mem_open unencoded: unconstrained posix_typed_mem_open is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:shm_open|shm_unlink)\\s*\\(", body)) {
        return std::format("shm unencoded: unconstrained shm_open is not a proof ({})", engine);
    }
    if (prism::re_search("\\bposix_spawn(?:_file_actions|attr)_init\\s*\\(", body)) {
        return std::format("posix_spawn_file_actions_init unencoded: unconstrained posix_spawn_file_actions_init is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:posix_spawnp|posix_spawn)\\s*\\(", body)) {
        return std::format("posix_spawn unencoded: unconstrained posix_spawn is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:globfree|glob)\\s*\\(", body)) {
        return std::format("glob unencoded: unconstrained glob is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:fseek|ftell|rewind|fgetpos|fsetpos)\\s*\\(", body)) {
        return std::format("fseek unencoded: unconstrained fseek is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:nanosleep|usleep|sleep)\\s*\\(", body)) {
        return std::format("sleep unencoded: unconstrained sleep is not a proof ({})", engine);
    }
    if (prism::re_search("\\bfaccessat2\\s*\\(", body)) {
        return std::format("faccessat2 unencoded: unconstrained faccessat2 is not a proof ({})", engine);
    }
    if (prism::re_search("\\bfaccessat\\s*\\(", body)) {
        return std::format("faccessat unencoded: unconstrained faccessat is not a proof ({})", engine);
    }
    if (prism::re_search("\\bvhangup\\s*\\(", body)) {
        return std::format("vhangup unencoded: unconstrained vhangup is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:at_)?quick_exit\\s*\\(", body)) {
        return std::format("quick_exit unencoded: unconstrained quick_exit is not a proof ({})", engine);
    }
    if (prism::re_search("\\beaccess\\s*\\(", body)) {
        return std::format("eaccess unencoded: unconstrained eaccess is not a proof ({})", engine);
    }
    if (prism::re_search("\\baccess\\s*\\(", body)) {
        return std::format("access unencoded: unconstrained access is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:getopt_long_only|getopt_long|getopt)\\s*\\(", body)) {
        return std::format("getopt unencoded: unconstrained getopt is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetosreldate\\s*\\(", body)) {
        return std::format("getosreldate unencoded: unconstrained getosreldate is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetdomainname\\s*\\(", body)) {
        return std::format("getdomainname unencoded: unconstrained getdomainname is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:uname|gethostname)\\s*\\(", body)) {
        return std::format("uname unencoded: unconstrained uname is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:sendfile|copy_file_range)\\s*\\(", body)) {
        return std::format("sendfile unencoded: unconstrained sendfile is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:preadv2|pwritev2|preadv|pwritev)\\s*\\(", body)) {
        return std::format("preadv unencoded: unconstrained preadv is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:timerfd_settime|timerfd_gettime)\\s*\\(", body)) {
        return std::format("timerfd_settime unencoded: unconstrained timerfd_settime is not a proof ({})", engine);
    }
    if (prism::re_search("\\beventfd_(?:read|write)\\s*\\(", body)) {
        return std::format("eventfd_read unencoded: unconstrained eventfd_read is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:memfd_create|eventfd|timerfd_create)\\s*\\(", body)) {
        return std::format("memfd unencoded: unconstrained memfd_create is not a proof ({})", engine);
    }
    if (prism::re_search("\\barch_prctl\\s*\\(", body)) {
        return std::format("arch_prctl unencoded: unconstrained arch_prctl is not a proof ({})", engine);
    }
    if (prism::re_search("\\bprocctl\\s*\\(", body)) {
        return std::format("procctl unencoded: unconstrained procctl is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:prctl|ptrace)\\s*\\(", body)) {
        return std::format("prctl unencoded: unconstrained prctl is not a proof ({})", engine);
    }
    if (prism::re_search("\\bktrace\\s*\\(", body)) {
        return std::format("ktrace unencoded: unconstrained ktrace is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:tcgetattr|tcsetattr|cfmakeraw)\\s*\\(", body)) {
        return std::format("tcgetattr unencoded: unconstrained tcgetattr is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetpagesizes\\s*\\(", body)) {
        return std::format("getpagesizes unencoded: unconstrained getpagesizes is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetpagesize(?!s)\\s*\\(", body)) {
        return std::format("getpagesize unencoded: unconstrained getpagesize is not a proof ({})", engine);
    }
    if (prism::re_search("\\blpathconf\\s*\\(", body)) {
        return std::format("lpathconf unencoded: unconstrained lpathconf is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:sysconf|fpathconf|pathconf)\\s*\\(", body)) {
        return std::format("sysconf unencoded: unconstrained sysconf is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetrusage\\s*\\(", body)) {
        return std::format("getrusage unencoded: unconstrained getrusage is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:nftw|ftw)\\s*\\(", body)) {
        return std::format("nftw unencoded: unconstrained nftw is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:wordexp|wordfree)\\s*\\(", body)) {
        return std::format("wordexp unencoded: unconstrained wordexp is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:get|set)loginclass\\s*\\(", body)) {
        return std::format("loginclass unencoded: unconstrained getloginclass is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:login_getclass|setusercontext)\\s*\\(", body)) {
        return std::format("login_getclass unencoded: unconstrained login_getclass is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsetlogin\\s*\\(", body)) {
        return std::format("setlogin unencoded: unconstrained setlogin is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:getlogin_r|getlogin|ttyname_r|ttyname)\\s*\\(", body)) {
        return std::format("getlogin unencoded: unconstrained getlogin is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:inet_pton|inet_ntop|inet_aton)\\s*\\(", body)) {
        return std::format("inet_pton unencoded: unconstrained inet_pton is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmseal\\s*\\(", body)) {
        return std::format("mseal unencoded: unconstrained mseal is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmlock2\\s*\\(", body)) {
        return std::format("mlock2 unencoded: unconstrained mlock2 is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:munlockall|mlockall|munlock|mlock)\\s*\\(", body)) {
        return std::format("mlock unencoded: unconstrained mlock is not a proof ({})", engine);
    }
    if (prism::re_search("\\bposix_fadvise(?:64)?\\s*\\(", body)) {
        return std::format("posix_fadvise unencoded: unconstrained posix_fadvise is not a proof ({})", engine);
    }
    if (prism::re_search("\\breadahead\\s*\\(", body)) {
        return std::format("readahead unencoded: unconstrained readahead is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:posix_madvise|madvise)\\s*\\(", body)) {
        return std::format("madvise unencoded: unconstrained madvise is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:vmsplice|splice)\\s*\\(", body)) {
        return std::format("splice unencoded: unconstrained splice is not a proof ({})", engine);
    }
    if (prism::re_search("\\binotify_rm_watch\\s*\\(", body)) {
        return std::format("inotify_rm_watch unencoded: unconstrained inotify_rm_watch is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:inotify_init1|inotify_init|inotify_add_watch)\\s*\\(", body)) {
        return std::format("inotify unencoded: unconstrained inotify is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:fdatasync|fsync)\\s*\\(", body)) {
        return std::format("fsync unencoded: unconstrained fsync is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:getrandom|getentropy)\\s*\\(", body)) {
        return std::format("getrandom unencoded: unconstrained getrandom is not a proof ({})", engine);
    }
    if (prism::re_search("\\barc4random(?:_buf|_uniform)?\\s*\\(", body)) {
        return std::format("arc4random unencoded: unconstrained arc4random is not a proof ({})", engine);
    }
    if (prism::re_search("\\bissetugid\\s*\\(", body)) {
        return std::format("issetugid unencoded: unconstrained issetugid is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:getdelim|getline)\\s*\\(", body)) {
        return std::format("getline unencoded: unconstrained getline is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:vasprintf|asprintf)\\s*\\(", body)) {
        return std::format("asprintf unencoded: unconstrained asprintf is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:strlcpy|strlcat)\\s*\\(", body)) {
        return std::format("strlcpy unencoded: unconstrained strlcpy is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:explicit_bzero|memset_s|explicit_memset)\\s*\\(", body)) {
        return std::format("explicit_bzero unencoded: unconstrained explicit_bzero is not a proof ({})", engine);
    }
    if (prism::re_search("\\btimingsafe_(?:bcmp|memcmp)\\s*\\(", body)) {
        return std::format("timingsafe_bcmp unencoded: unconstrained timingsafe_bcmp is not a proof ({})", engine);
    }
    if (prism::re_search("\\bisatty\\s*\\(", body)) {
        return std::format("isatty unencoded: unconstrained isatty is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:posix_openpt|ptsname_r|ptsname|grantpt|unlockpt)\\s*\\(", body)) {
        return std::format("ptsname unencoded: unconstrained ptsname is not a proof ({})", engine);
    }
    if (prism::re_search("\\bnmount\\s*\\(", body)) {
        return std::format("nmount unencoded: unconstrained nmount is not a proof ({})", engine);
    }
    if (prism::re_search("\\bunmount\\b", body)) {
        return std::format("unmount unencoded: unconstrained unmount is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:umount2|umount|mount)\\s*\\(", body)) {
        return std::format("mount unencoded: unconstrained mount is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:open_wmemstream|open_memstream|fmemopen)\\s*\\(", body)) {
        return std::format("fmemopen unencoded: unconstrained fmemopen is not a proof ({})", engine);
    }
    if (prism::re_search("\\bscandir\\s*\\(", body)) {
        return std::format("scandir unencoded: unconstrained scandir is not a proof ({})", engine);
    }
    if (prism::re_search("\\bextattr_(?:set|get|delete|list)_(?:file|fd|link)\\s*\\(", body)) {
        return std::format("extattr unencoded: unconstrained extattr_set_file is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:set|get|list|remove)xattrat\\s*\\(", body)) {
        return std::format("setxattrat unencoded: unconstrained setxattrat is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:lsetxattr|fsetxattr|setxattr|listxattr|removexattr|getxattr)\\s*\\(", body)) {
        return std::format("setxattr unencoded: unconstrained setxattr is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:sched_setattr|sched_getattr)\\s*\\(", body)) {
        return std::format("sched_setattr unencoded: unconstrained sched_setattr is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsched_yield\\s*\\(", body)) {
        return std::format("sched_yield unencoded: unconstrained sched_yield is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpthread_yield\\s*\\(", body)) {
        return std::format("pthread_yield unencoded: unconstrained pthread_yield is not a proof ({})", engine);
    }
    if (prism::re_search("\\bcpuset_(?:set|get)affinity\\s*\\(", body)) {
        return std::format("cpuset unencoded: unconstrained cpuset_setaffinity is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsched_get_priority_(?:max|min)\\s*\\(", body)) {
        return std::format("sched_get_priority_max unencoded: unconstrained sched_get_priority_max is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:sched_setaffinity|sched_getaffinity)\\s*\\(", body)) {
        return std::format("sched unencoded: unconstrained sched_setaffinity is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:sched_setscheduler|sched_getscheduler|sched_setparam|sched_getparam)\\s*\\(", body)) {
        return std::format("sched_setscheduler unencoded: unconstrained sched_setscheduler is not a proof ({})", engine);
    }
    if (prism::re_search("\\blio_listio\\s*\\(", body)) {
        return std::format("lio_listio unencoded: unconstrained lio_listio is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:aio_suspend|aio_return|aio_error|aio_write|aio_read)\\s*\\(", body)) {
        return std::format("aio unencoded: unconstrained aio_read is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:io_uring_register|io_uring_setup|io_uring_enter)\\s*\\(", body)) {
        return std::format("io_uring unencoded: unconstrained io_uring_setup is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:capset|capget)\\s*\\(", body)) {
        return std::format("capset unencoded: unconstrained capset is not a proof ({})", engine);
    }
    if (prism::re_search("\\bstatx\\s*\\(", body)) {
        return std::format("statx unencoded: unconstrained statx is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:pidfd_send_signal|pidfd_getfd|pidfd_open)\\s*\\(", body)) {
        return std::format("pidfd unencoded: unconstrained pidfd_open is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:fanotify_init|fanotify_mark)\\s*\\(", body)) {
        return std::format("fanotify unencoded: unconstrained fanotify_init is not a proof ({})", engine);
    }
    if (prism::re_search("\\bseccomp\\s*\\(", body)) {
        return std::format("seccomp unencoded: unconstrained seccomp is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:getgrnam|getgrgid|getspnam)\\s*\\(", body)) {
        return std::format("getgrnam unencoded: unconstrained getgrnam is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:posix_fallocate|fallocate)\\s*\\(", body)) {
        return std::format("fallocate unencoded: unconstrained posix_fallocate is not a proof ({})", engine);
    }
    if (prism::re_search("\\bclose_range\\s*\\(", body)) {
        return std::format("close_range unencoded: unconstrained close_range is not a proof ({})", engine);
    }
    if (prism::re_search("\\bclosefrom\\s*\\(", body)) {
        return std::format("closefrom unencoded: unconstrained closefrom is not a proof ({})", engine);
    }
    if (prism::re_search("\\blsm_(?:get_self_attr|set_self_attr|list_modules)\\s*\\(", body)) {
        return std::format("lsm_get_self_attr unencoded: unconstrained lsm_get_self_attr is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:landlock_create_ruleset|landlock_add_rule|landlock_restrict_self)\\s*\\(", body)) {
        return std::format("landlock unencoded: unconstrained landlock_create_ruleset is not a proof ({})", engine);
    }
    if (prism::re_search("\\brtprio(?:_thread)?\\s*\\(", body)) {
        return std::format("rtprio unencoded: unconstrained rtprio is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:getpriority|setpriority)\\s*\\(", body)) {
        return std::format("getpriority unencoded: unconstrained getpriority is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsignalfd\\s*\\(", body)) {
        return std::format("signalfd unencoded: unconstrained signalfd is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsigaction\\s*\\(", body)) {
        return std::format("sigaction unencoded: unconstrained sigaction is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpthread_sigmask\\s*\\(", body)) {
        return std::format("pthread_sigmask unencoded: unconstrained pthread_sigmask is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsig(?:procmask|suspend)\\s*\\(", body)) {
        return std::format("sigprocmask unencoded: unconstrained sigprocmask is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsig(?:waitinfo|timedwait|pending|wait)\\s*\\(", body)) {
        return std::format("sigwait unencoded: unconstrained sigwait is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsigaltstack\\s*\\(", body)) {
        return std::format("sigaltstack unencoded: unconstrained sigaltstack is not a proof ({})", engine);
    }
    if (prism::re_search("\\bbpf\\s*\\(", body)) {
        return std::format("bpf unencoded: unconstrained bpf is not a proof ({})", engine);
    }
    if (prism::re_search("\\buserfaultfd\\s*\\(", body)) {
        return std::format("userfaultfd unencoded: unconstrained userfaultfd is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetpass\\s*\\(", body)) {
        return std::format("getpass unencoded: unconstrained getpass is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetgrouplist\\s*\\(", body)) {
        return std::format("getgrouplist unencoded: unconstrained getgrouplist is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetgroups\\s*\\(", body)) {
        return std::format("getgroups unencoded: unconstrained getgroups is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:initgroups|setgroups)\\s*\\(", body)) {
        return std::format("initgroups unencoded: unconstrained initgroups is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:unshare|setns|clone)\\s*\\(", body)) {
        return std::format("unshare unencoded: unconstrained unshare/clone is not a proof ({})", engine);
    }
    if (prism::re_search("\\bopenat2\\s*\\(", body)) {
        return std::format("openat2 unencoded: unconstrained openat2 is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:sendmmsg|recvmmsg)\\s*\\(", body)) {
        return std::format("sendmmsg unencoded: unconstrained sendmmsg is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:name_to_handle_at|open_by_handle_at)\\s*\\(", body)) {
        return std::format("name_to_handle unencoded: unconstrained name_to_handle_at is not a proof ({})", engine);
    }
    if (prism::re_search("\\bprocess_madvise\\s*\\(", body)) {
        return std::format("process_madvise unencoded: unconstrained process_madvise is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpersonality\\s*\\(", body)) {
        return std::format("personality unencoded: unconstrained personality is not a proof ({})", engine);
    }
    if (prism::re_search("\\bquotactl\\s*\\(", body)) {
        return std::format("quotactl unencoded: unconstrained quotactl is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpivot_root\\s*\\(", body)) {
        return std::format("pivot_root unencoded: unconstrained pivot_root is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmembarrier\\s*\\(", body)) {
        return std::format("membarrier unencoded: unconstrained membarrier is not a proof ({})", engine);
    }
    if (prism::re_search("\\bpkey_alloc\\s*\\(", body)) {
        return std::format("pkey_alloc unencoded: unconstrained pkey_alloc is not a proof ({})", engine);
    }
    if (prism::re_search("\\bstatfs\\s*\\(", body)) {
        return std::format("statfs unencoded: unconstrained statfs is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetmntinfo\\s*\\(", body)) {
        return std::format("getmntinfo unencoded: unconstrained getmntinfo is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetvfsbyname\\s*\\(", body)) {
        return std::format("getvfsbyname unencoded: unconstrained getvfsbyname is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:get|set|end)fsent\\s*\\(", body)) {
        return std::format("getfsent unencoded: unconstrained getfsent is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetfsstat\\s*\\(", body)) {
        return std::format("getfsstat unencoded: unconstrained getfsstat is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsyncfs\\s*\\(", body)) {
        return std::format("syncfs unencoded: unconstrained syncfs is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:prlimit64|prlimit)\\s*\\(", body)) {
        return std::format("prlimit unencoded: unconstrained prlimit is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:migrate_pages|move_pages)\\s*\\(", body)) {
        return std::format("move_pages unencoded: unconstrained move_pages is not a proof ({})", engine);
    }
    if (prism::re_search("\\bprocess_vm_readv\\s*\\(", body)) {
        return std::format("process_vm_readv unencoded: unconstrained process_vm_readv is not a proof ({})", engine);
    }
    if (prism::re_search("\\bperf_event_open\\s*\\(", body)) {
        return std::format("perf_event_open unencoded: unconstrained perf_event_open is not a proof ({})", engine);
    }
    if (prism::re_search("\\bclone3\\s*\\(", body)) {
        return std::format("clone3 unencoded: unconstrained clone3 is not a proof ({})", engine);
    }
    if (prism::re_search("\\bkcmp\\s*\\(", body)) {
        return std::format("kcmp unencoded: unconstrained kcmp is not a proof ({})", engine);
    }
    if (prism::re_search("\\bkeyctl\\s*\\(", body)) {
        return std::format("keyctl unencoded: unconstrained keyctl is not a proof ({})", engine);
    }
    if (prism::re_search("\\bopen_tree_attr\\s*\\(", body)) {
        return std::format("open_tree_attr unencoded: unconstrained open_tree_attr is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:fsopen|fsmount|open_tree|move_mount|fspick|fsconfig)\\s*\\(", body)) {
        return std::format("fsopen unencoded: unconstrained fsopen is not a proof ({})", engine);
    }
    if (prism::re_search("\\bprocess_mrelease\\s*\\(", body)) {
        return std::format("process_mrelease unencoded: unconstrained process_mrelease is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmemfd_secret\\s*\\(", body)) {
        return std::format("memfd_secret unencoded: unconstrained memfd_secret is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:ioprio_set|ioprio_get)\\s*\\(", body)) {
        return std::format("ioprio unencoded: unconstrained ioprio is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmq_open\\s*\\(", body)) {
        return std::format("mq_open unencoded: unconstrained mq_open is not a proof ({})", engine);
    }
    if (prism::re_search("\\bshmget\\s*\\(", body)) {
        return std::format("shmget unencoded: unconstrained shmget is not a proof ({})", engine);
    }
    if (prism::re_search("\\b_umtx_op\\s*\\(", body)) {
        return std::format("_umtx_op unencoded: unconstrained _umtx_op is not a proof ({})", engine);
    }
    if (prism::re_search("\\bfutex_waitv\\s*\\(", body)) {
        return std::format("futex_waitv unencoded: unconstrained futex_waitv is not a proof ({})", engine);
    }
    if (prism::re_search("\\bfutex_(?:wake|wait|requeue)\\s*\\(", body)) {
        return std::format("futex_wait unencoded: unconstrained futex_wait is not a proof ({})", engine);
    }
    if (prism::re_search("\\bfutex\\s*\\(", body)) {
        return std::format("futex unencoded: unconstrained futex is not a proof ({})", engine);
    }
    if (prism::re_search("\\badjtimex\\s*\\(", body)) {
        return std::format("adjtimex unencoded: unconstrained adjtimex is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:ntp_)?adjtime\\s*\\(", body)) {
        return std::format("adjtime unencoded: unconstrained adjtime is not a proof ({})", engine);
    }
    if (prism::re_search("\\bntp_gettime\\s*\\(", body)) {
        return std::format("ntp_gettime unencoded: unconstrained ntp_gettime is not a proof ({})", engine);
    }
    if (prism::re_search("\\brevoke\\s*\\(", body)) {
        return std::format("revoke unencoded: unconstrained revoke is not a proof ({})", engine);
    }
    if (prism::re_search("\\bjail(?:_attach|_get|_set|_remove)?\\s*\\(", body)) {
        return std::format("jail unencoded: unconstrained jail is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:fflagstostr|strtofflags)\\s*\\(", body)) {
        return std::format("fflagstostr unencoded: unconstrained fflagstostr is not a proof ({})", engine);
    }
    if (prism::re_search("\\bstrmode\\s*\\(", body)) {
        return std::format("strmode unencoded: unconstrained strmode is not a proof ({})", engine);
    }
    if (prism::re_search("\\bstrtonum\\s*\\(", body)) {
        return std::format("strtonum unencoded: unconstrained strtonum is not a proof ({})", engine);
    }
    if (prism::re_search("\\breallocarray\\s*\\(", body)) {
        return std::format("reallocarray unencoded: unconstrained reallocarray is not a proof ({})", engine);
    }
    if (prism::re_search("\\breallocf\\s*\\(", body)) {
        return std::format("reallocf unencoded: unconstrained reallocf is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:get|set)progname\\s*\\(", body)) {
        return std::format("getprogname unencoded: unconstrained getprogname is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:setproctitle|daemon)\\s*\\(", body)) {
        return std::format("daemon unencoded: unconstrained daemon is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:auditon|getaudit|setaudit|auditctl)\\s*\\(", body)) {
        return std::format("auditon unencoded: unconstrained auditon is not a proof ({})", engine);
    }
    if (prism::re_search("\\bkinfo_get(?:proc|file|vmmap)\\s*\\(", body)) {
        return std::format("kinfo_getproc unencoded: unconstrained kinfo_getproc is not a proof ({})", engine);
    }
    if (prism::re_search("\\bkvm_(?:open|openfiles|getprocs|close|nlist)\\s*\\(", body)) {
        return std::format("kvm_open unencoded: unconstrained kvm_open is not a proof ({})", engine);
    }
    if (prism::re_search("\\buuidgen\\s*\\(", body)) {
        return std::format("uuidgen unencoded: unconstrained uuidgen is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsetfib\\s*\\(", body)) {
        return std::format("setfib unencoded: unconstrained setfib is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsethostname\\s*\\(", body)) {
        return std::format("sethostname unencoded: unconstrained sethostname is not a proof ({})", engine);
    }
    if (prism::re_search("\\breboot\\s*\\(", body)) {
        return std::format("reboot unencoded: unconstrained reboot is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:swapon|swapoff)\\s*\\(", body)) {
        return std::format("swapon unencoded: unconstrained swapon is not a proof ({})", engine);
    }
    if (prism::re_search("\\bacct\\s*\\(", body)) {
        return std::format("acct unencoded: unconstrained acct is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:ioperm|iopl)\\s*\\(", body)) {
        return std::format("ioperm unencoded: unconstrained ioperm is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmincore\\s*\\(", body)) {
        return std::format("mincore unencoded: unconstrained mincore is not a proof ({})", engine);
    }
    if (prism::re_search("\\brseq\\s*\\(", body)) {
        return std::format("rseq unencoded: unconstrained rseq is not a proof ({})", engine);
    }
    if (prism::re_search("\\btimer_create\\s*\\(", body)) {
        return std::format("timer_create unencoded: unconstrained timer_create is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsemget\\s*\\(", body)) {
        return std::format("semget unencoded: unconstrained semget is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmsgget\\s*\\(", body)) {
        return std::format("msgget unencoded: unconstrained msgget is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsyslog\\s*\\(", body)) {
        return std::format("syslog unencoded: unconstrained syslog is not a proof ({})", engine);
    }
    if (prism::re_search("\\bklogctl\\s*\\(", body)) {
        return std::format("klogctl unencoded: unconstrained klogctl is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmount_setattr\\s*\\(", body)) {
        return std::format("mount_setattr unencoded: unconstrained mount_setattr is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetcpu\\s*\\(", body)) {
        return std::format("getcpu unencoded: unconstrained getcpu is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:init_module|finit_module|delete_module)\\s*\\(", body)) {
        return std::format("init_module unencoded: unconstrained init_module is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:kexec_load|kexec_file_load)\\s*\\(", body)) {
        return std::format("kexec unencoded: unconstrained kexec_load is not a proof ({})", engine);
    }
    if (prism::re_search("\\bquotactl_fd\\s*\\(", body)) {
        return std::format("quotactl_fd unencoded: unconstrained quotactl_fd is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:pkey_free|pkey_mprotect)\\s*\\(", body)) {
        return std::format("pkey_free unencoded: unconstrained pkey_free is not a proof ({})", engine);
    }
    if (prism::re_search("\\btgkill\\s*\\(", body)) {
        return std::format("tgkill unencoded: unconstrained tgkill is not a proof ({})", engine);
    }
    if (prism::re_search("\\badd_key\\s*\\(", body)) {
        return std::format("add_key unencoded: unconstrained add_key is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsemctl\\s*\\(", body)) {
        return std::format("semctl unencoded: unconstrained semctl is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmsgctl\\s*\\(", body)) {
        return std::format("msgctl unencoded: unconstrained msgctl is not a proof ({})", engine);
    }
    if (prism::re_search("\\bshmctl\\s*\\(", body)) {
        return std::format("shmctl unencoded: unconstrained shmctl is not a proof ({})", engine);
    }
    if (prism::re_search("\\btimer_settime\\s*\\(", body)) {
        return std::format("timer_settime unencoded: unconstrained timer_settime is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsetdomainname\\s*\\(", body)) {
        return std::format("setdomainname unencoded: unconstrained setdomainname is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:io_submit|io_getevents)\\s*\\(", body)) {
        return std::format("io_submit unencoded: unconstrained io_submit is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:io_setup|io_destroy|io_cancel|io_pgetevents)\\s*\\(", body)) {
        return std::format("io_setup unencoded: unconstrained io_setup is not a proof ({})", engine);
    }
    if (prism::re_search("\\brequest_key\\s*\\(", body)) {
        return std::format("request_key unencoded: unconstrained request_key is not a proof ({})", engine);
    }
    if (prism::re_search("\\btkill\\s*\\(", body)) {
        return std::format("tkill unencoded: unconstrained tkill is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:timer_delete|timer_gettime|timer_getoverrun)\\s*\\(", body)) {
        return std::format("timer_delete unencoded: unconstrained timer_delete is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:mq_unlink|mq_timedsend|mq_timedreceive|mq_notify|mq_getsetattr)\\s*\\(", body)) {
        return std::format("mq_unlink unencoded: unconstrained mq_unlink is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:shmat|shmdt)\\s*\\(", body)) {
        return std::format("shmat unencoded: unconstrained shmat is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:semop|semtimedop)\\s*\\(", body)) {
        return std::format("semop unencoded: unconstrained semop is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:msgsnd|msgrcv)\\s*\\(", body)) {
        return std::format("msgsnd unencoded: unconstrained msgsnd is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsync_file_range\\s*\\(", body)) {
        return std::format("sync_file_range unencoded: unconstrained sync_file_range is not a proof ({})", engine);
    }
    if (prism::re_search("\\bremap_file_pages\\s*\\(", body)) {
        return std::format("remap_file_pages unencoded: unconstrained remap_file_pages is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:msync|mremap)\\s*\\(", body)) {
        return std::format("msync unencoded: unconstrained msync is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsocketpair\\s*\\(", body)) {
        return std::format("socketpair unencoded: unconstrained socketpair is not a proof ({})", engine);
    }
    if (prism::re_search("\\bsysinfo\\s*\\(", body)) {
        return std::format("sysinfo unencoded: unconstrained sysinfo is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgettid\\s*\\(", body)) {
        return std::format("gettid unencoded: unconstrained gettid is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:setitimer|getitimer)\\s*\\(", body)) {
        return std::format("setitimer unencoded: unconstrained setitimer is not a proof ({})", engine);
    }
    if (prism::re_search("\\bnice\\s*\\(", body)) {
        return std::format("nice unencoded: unconstrained nice is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetdirentries\\s*\\(", body)) {
        return std::format("getdirentries unencoded: unconstrained getdirentries is not a proof ({})", engine);
    }
    if (prism::re_search("\\bgetdents(?:64)?\\s*\\(", body)) {
        return std::format("getdents unencoded: unconstrained getdents is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:utimensat|futimens|utimes)\\s*\\(", body)) {
        return std::format("utimensat unencoded: unconstrained utimensat is not a proof ({})", engine);
    }
    if (prism::re_search("\\blinkat\\s*\\(", body)) {
        return std::format("linkat unencoded: unconstrained linkat is not a proof ({})", engine);
    }
    if (prism::re_search("\\bset_mempolicy_home_node\\s*\\(", body)) {
        return std::format("set_mempolicy_home_node unencoded: unconstrained set_mempolicy_home_node is not a proof ({})", engine);
    }
    if (prism::re_search("\\b(?:mbind|set_mempolicy|get_mempolicy)\\s*\\(", body)) {
        return std::format("mbind unencoded: unconstrained mbind is not a proof ({})", engine);
    }
    if (prism::re_search("\\bcachestat\\s*\\(", body)) {
        return std::format("cachestat unencoded: unconstrained cachestat is not a proof ({})", engine);
    }
    if (prism::re_search("\\bmap_shadow_stack\\s*\\(", body)) {
        return std::format("map_shadow_stack unencoded: unconstrained map_shadow_stack is not a proof ({})", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*pmr\\b|\\bpmr\\s*::", body)) {
        return std::format("C++ pmr unencoded: {} is not a pmr model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?u8string(?:_view)?\\b", body)) {
        return std::format("C++ u8string unencoded: {} is not a u8string model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?unordered_multimap\\s*<", body)) {
        return std::format("C++ unordered_multimap unencoded: {} is not an unordered_multimap model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?unordered_multiset\\s*<", body)) {
        return std::format("C++ unordered_multiset unencoded: {} is not an unordered_multiset model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?shared_lock\\b", body)) {
        return std::format("C++ shared_lock unencoded: {} is not a shared_lock model", engine);
    }
    if (prism::re_search("\\batomic_(?:thread|signal)_fence\\s*\\(", body)) {
        return std::format("C++ atomic_thread_fence unencoded: {} is not an atomic_thread_fence model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?atomic_flag\\b", body)) {
        return std::format("C++ atomic_flag unencoded: {} is not an atomic_flag model", engine);
    }
    if (prism::re_search("\\brecursive_timed_mutex\\b", body)) {
        return std::format("C++ recursive_timed_mutex unencoded: {} is not a recursive_timed_mutex model", engine);
    }
    if (prism::re_search("\\brecursive_mutex\\b", body)) {
        return std::format("C++ recursive_mutex unencoded: {} is not a recursive_mutex model", engine);
    }
    if (prism::re_search("\\btimed_mutex\\b", body)) {
        return std::format("C++ timed_mutex unencoded: {} is not a timed_mutex model", engine);
    }
    if (prism::re_search("\\b(?:ifstream|ofstream|fstream)\\b", body)) {
        return std::format("C++ fstream unencoded: {} is not an fstream model", engine);
    }
    if (prism::re_search("\\bthis_thread\\b", body)) {
        return std::format("C++ this_thread unencoded: {} is not a this_thread model", engine);
    }
    if (prism::re_search("\\bcall_once\\s*\\(", body)) {
        return std::format("C++ call_once unencoded: {} is not a call_once model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?tuple\\s*<", body)) {
        return std::format("C++ tuple unencoded: {} is not a tuple model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?deque\\s*<", body)) {
        return std::format("C++ deque unencoded: {} is not a deque model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?forward_list\\s*<", body)) {
        return std::format("C++ forward_list unencoded: {} is not a forward_list model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*list\\s*<", body)) {
        return std::format("C++ std::list unencoded: {} is not a list model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?unordered_map\\s*<", body)) {
        return std::format("C++ unordered_map unencoded: {} is not an unordered_map model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*map\\s*<", body)) {
        return std::format("C++ std::map unencoded: {} is not a map model", engine);
    }
    if (prism::re_search("\\bunordered_set\\s*<", body)) {
        return std::format("C++ unordered_set unencoded: {} is not an unordered_set model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*set\\s*<", body)) {
        return std::format("C++ std::set unencoded: {} is not a set model", engine);
    }
    if (prism::re_search("\\bpriority_queue\\s*<", body)) {
        return std::format("C++ priority_queue unencoded: {} is not a priority_queue model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*queue\\s*<", body)) {
        return std::format("C++ std::queue unencoded: {} is not a queue model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*stack\\s*<", body)) {
        return std::format("C++ std::stack unencoded: {} is not a stack model", engine);
    }
    if (prism::re_search("\\bto_array\\s*[<(]", body)) {
        return std::format("C++ to_array unencoded: {} is not a to_array model", engine);
    }
    if (prism::re_search("\\bfrom_range\\b", body)) {
        return std::format("C++ from_range unencoded: {} is not a from_range model", engine);
    }
    if (prism::re_search("\\branges\\s*::\\s*to\\s*[<(]", body)) {
        return std::format("C++ ranges::to unencoded: {} is not a ranges::to model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*array\\s*<", body)) {
        return std::format("C++ std::array unencoded: {} is not an array model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?wstring_convert\\b", body)) {
        return std::format("C++ wstring_convert unencoded: {} is not a wstring_convert model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?wstring(?:_view)?\\b", body)) {
        return std::format("C++ wstring unencoded: {} is not a wstring model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?multimap\\s*<", body)) {
        return std::format("C++ multimap unencoded: {} is not a multimap model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?multiset\\s*<", body)) {
        return std::format("C++ multiset unencoded: {} is not a multiset model", engine);
    }
    if (prism::re_search("\\bbinary_semaphore\\b", body)) {
        return std::format("C++ binary_semaphore unencoded: {} is not a binary_semaphore model", engine);
    }
    if (prism::re_search("\\berror_category\\b", body)) {
        return std::format("C++ error_category unencoded: {} is not an error_category model", engine);
    }
    if (prism::re_search("\\bsystem_error\\b", body)) {
        return std::format("C++ system_error unencoded: {} is not a system_error model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?error_code\\b", body)) {
        return std::format("C++ error_code unencoded: {} is not an error_code model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*apply\\s*\\(", body)) {
        return std::format("C++ std::apply unencoded: {} is not an apply model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*invoke\\s*\\(", body)) {
        return std::format("C++ std::invoke unencoded: {} is not an invoke model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*endian\\b", body)) {
        return std::format("C++ std::endian unencoded: {} is not an endian model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*rot[lr]\\s*\\(", body)) {
        return std::format("C++ std::rotl unencoded: {} is not a rotl model", engine);
    }
    if (prism::re_search("\\b(?:bit_ceil|bit_floor|has_single_bit|std\\s*::\\s*popcount)\\s*\\(", body)) {
        return std::format("C++ bit_ceil unencoded: {} is not a bit_ceil model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*bit_width\\s*\\(", body)) {
        return std::format("C++ std::bit_width unencoded: {} is not a bit_width model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*gcd\\s*\\(", body)) {
        return std::format("C++ std::gcd unencoded: {} is not a gcd model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*lcm\\s*\\(", body)) {
        return std::format("C++ std::lcm unencoded: {} is not a lcm model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*clamp\\s*\\(", body)) {
        return std::format("C++ std::clamp unencoded: {} is not a clamp model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*exchange\\s*\\(", body)) {
        return std::format("C++ std::exchange unencoded: {} is not an exchange model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*to_address\\s*\\(", body)) {
        return std::format("C++ std::to_address unencoded: {} is not a to_address model", engine);
    }
    if (prism::re_search("\\b(?:construct_at|destroy_at)\\s*\\(", body)) {
        return std::format("C++ construct_at unencoded: {} is not a construct_at model", engine);
    }
    if (prism::re_search("\\bdestroy_n\\s*\\(", body)) {
        return std::format("C++ destroy_n unencoded: {} is not a destroy_n model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*addressof\\s*\\(", body)) {
        return std::format("C++ std::addressof unencoded: {} is not an addressof model", engine);
    }
    if (prism::re_search("\\bassume_aligned\\s*\\(", body)) {
        return std::format("C++ assume_aligned unencoded: {} is not an assume_aligned model", engine);
    }
    if (prism::re_search("\\bas_rvalue(?:_view)?\\b", body)) {
        return std::format("C++ as_rvalue unencoded: {} is not an as_rvalue model", engine);
    }
    if (prism::re_search("\\bas_const\\s*\\(", body)) {
        return std::format("C++ as_const unencoded: {} is not an as_const model", engine);
    }
    if (prism::re_search("\\btransform_(?:inclusive|exclusive)_scan\\s*\\(", body)) {
        return std::format("C++ transform_inclusive_scan unencoded: {} is not a transform_inclusive_scan model", engine);
    }
    if (prism::re_search("\\bexclusive_scan\\s*\\(", body)) {
        return std::format("C++ exclusive_scan unencoded: {} is not an exclusive_scan model", engine);
    }
    if (prism::re_search("\\binclusive_scan\\s*\\(", body)) {
        return std::format("C++ inclusive_scan unencoded: {} is not an inclusive_scan model", engine);
    }
    if (prism::re_search("\\btransform_reduce\\s*\\(", body)) {
        return std::format("C++ transform_reduce unencoded: {} is not a transform_reduce model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*reduce\\s*\\(", body)) {
        return std::format("C++ std::reduce unencoded: {} is not a reduce model", engine);
    }
    if (prism::re_search("\\buninitialized_(?:fill(?:_n)?|default_construct(?:_n)?)\\s*\\(", body)) {
        return std::format("C++ uninitialized_fill unencoded: {} is not an uninitialized_fill model", engine);
    }
    if (prism::re_search("\\buninitialized_value_construct(?:_n)?\\s*\\(", body)) {
        return std::format("C++ uninitialized_value_construct unencoded: {} is not an uninitialized_value_construct model", engine);
    }
    if (prism::re_search("\\buninitialized_(?:copy|move)(?:_n)?\\s*\\(", body)) {
        return std::format("C++ uninitialized_copy unencoded: {} is not an uninitialized_copy model", engine);
    }
    if (prism::re_search("\\b(?:add_sat|sub_sat|mul_sat|div_sat|saturate_cast)\\s*\\(", body)) {
        return std::format("C++ add_sat unencoded: {} is not an add_sat model", engine);
    }
    if (prism::re_search("\\bnontype\\b", body)) {
        return std::format("C++ nontype unencoded: {} is not a nontype model", engine);
    }
    if (prism::re_search("\\bis_layout_compatible\\b", body)) {
        return std::format("C++ is_layout_compatible unencoded: {} is not an is_layout_compatible model", engine);
    }
    if (prism::re_search("\\bis_pointer_interconvertible_(?:with_class|base_of)\\b", body)) {
        return std::format("C++ is_pointer_interconvertible unencoded: {} is not an is_pointer_interconvertible model", engine);
    }
    if (prism::re_search("\\bbasic_const_iterator\\b", body)) {
        return std::format("C++ basic_const_iterator unencoded: {} is not a basic_const_iterator model", engine);
    }
    if (prism::re_search("\\bis_corresponding_member\\b", body)) {
        return std::format("C++ is_corresponding_member unencoded: {} is not an is_corresponding_member model", engine);
    }
    if (prism::re_search("\\bforward_like\\b", body)) {
        return std::format("C++ forward_like unencoded: {} is not a forward_like model", engine);
    }
    if (prism::re_search("\\b(?:set|get)_terminate\\s*\\(", body)) {
        return std::format("C++ set_terminate unencoded: {} is not a set_terminate model", engine);
    }
    if (prism::re_search("\\bis_constant_evaluated\\s*\\(", body)) {
        return std::format("C++ is_constant_evaluated unencoded: {} is not an is_constant_evaluated model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*lerp\\s*\\(", body)) {
        return std::format("C++ std::lerp unencoded: {} is not a lerp model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*midpoint\\s*\\(", body)) {
        return std::format("C++ std::midpoint unencoded: {} is not a midpoint model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*(?:cmp_(?:less|greater|less_equal|greater_equal|equal_to|not_equal_to)|in_range)\\s*\\(", body)) {
        return std::format("C++ std::cmp_less unencoded: {} is not a cmp_less model", engine);
    }
    if (prism::re_search("\\bstd\\s*::\\s*count[lr]_(?:zero|one)\\s*\\(", body)) {
        return std::format("C++ std::countl_zero unencoded: {} is not a countl_zero model", engine);
    }
    if (prism::re_search("\\b(?:std\\s*::\\s*)?byteswap\\s*\\(", body)) {
        return std::format("C++ byteswap unencoded: {} is not a byteswap model", engine);
    }
    if (prism::re_search("\\b(?:wcscpy|wcscat|wcsncpy|wcsncat)\\s*\\(", body)) {
        return std::format("wide-string copy unencoded: unconstrained wcscpy is not a proof of the buffer ({})", engine);
    }
    if (prism::re_search("\\b(?:memcpy|memmove|mkstemp|mkstemps|mkdtemp|tmpnam|tempnam|tmpnam_r|chroot|jail_attach|jail_remove|jail_get|jail_set|jail|popen|pclose|umask|srand|srandom|mktemp|signal|pthread_atfork|pdfork|pdwait4|pdgetpid|rfork|pledge|unveil|cap_getmode|cap_getrights|cap_fcntls_limit|cap_ioctls_limit|cap_sandboxed|cap_enter|cap_rights_limit|cap_rights_get|sysctlbyname|sysctl|kqueue|kevent|pause|getcontext|setcontext|swapcontext|makecontext|pthread_attr_(?:init|destroy|setstacksize|setstack|setdetachstate|getstacksize|getstack|getdetachstate)|fork|vfork|execlp|execle|execl|execveat|execvpe|execvp|execve|execv|mmap|munmap|mprotect|minherit|sbrk|brk|cap_ioctls_limit|ioctl|modfind|modstat|modnext|modfnext|kldfirstmod|kldnextmod|kld_isloaded|kld_load|kldload|kldunload|kldfind|kldsym|kldstat|nfssvc|sysarch|getbootfile|devname_r|devname|dlfunc|dlvsym|dlopen|dlsym|dlclose|accept4|accept|fchmodat2|fchmodat|fchmod|chmod|lchflags|fchflags|chflags|setreuid|setregid|setresuid|setresgid|getresuid|getresgid|setfsuid|setfsgid|setpgid|setsid|getsid|setuid|seteuid|setgid|socket|listen|connect|unlinkat|unlink|mkfifo|mknodat|mknod|pipe2|pipe|dup3|dup2|dup|cap_fcntls_limit|fcntl|waitpid|waitid|pdwait4|wait6|wait4|wait3|wait|pselect|select|epoll_create1|epoll_create|epoll_pwait2|epoll_pwait|epoll_wait|epoll_ctl|ppoll|poll|sendmsg|recvmsg|sendto|recvfrom|send|recv|shutdown|kill_dependency|rt_tgsigqueueinfo|rt_si" "gqueueinfo|sigqueue|thr_kill2|thr_kill|thr_new|thr_self|thr_exit|thr_suspend|thr_wake|pthread_kill|kill|raise|alarm|getaddrinfo|freeaddrinfo|pthread_cancel|pthread_key_create|pthread_key_delete|pthread_setspecific|pthread_getspecific|pthread_join|pthread_detach|pthread_once|pthread_spin_(?:try)?(?:lock|unlock|init|destroy)|pthread_rwlock_(?:try|timed)?(?:rdlock|wrlock|unlock|init|destroy)|pthread_cond_(?:timedwait|wait|signal|broadcast|init|destroy)|pthread_barrier_(?:wait|init|destroy)|sem_timedwait|sem_trywait|sem_getvalue|sem_wait|sem_post|sem_init|sem_destroy|ksem_open|ksem_close|ksem_unlink|ksem_wait|ksem_post|sem_open|sem_close|sem_unlink|openat|flock|valloc|posix_memalign|aligned_alloc|fchown|lchown|chown|symlinkat|symlink|readlinkat|readlink|fts_open|fts_read|fts_children|fts_close|fts_set|fdopendir|opendir|readdir|closedir|setrlimit|getrlimit|getsockname|getpeername|getpeereid|getsockopt|setsockopt|lstat|fstatat|fstat|statmount|listmount|ustat|file_getattr|file_setattr|fhlinkat|fhlink|fhreadlink|getfhat|getfh|fhopen|fhstatfs|fhstat|stat|mkdirat|mkdir|rmdir|renameat2|renameat|rename|getpwuid|getpwnam|crypt_newhash|crypt_checkpass|crypt|clock_settime|clock_adjtime|clock_nanosleep|settimeofday|pthread_getcpuclockid|clock_getcpuclockid|clock_gettime|gettimeofday|posix_typed_mem_open|posix_typed_mem_get_info|shm_open|shm_unlink|posix_spawn_file_actions_init|posix_spawnattr_" "init|posix_spawnp|posix_spawn|globfree|glob|fseek|ftell|rewind|fgetpos|fsetpos|nanosleep|usleep|sleep|faccessat2|faccessat|vhangup|at_quick_exit|quick_exit|eaccess|access|getopt_long_only|getopt_long|getopt|getosreldate|uname|gethostname|sendfile|copy_file_range|preadv2|pwritev2|preadv|pwritev|memfd_create|eventfd_read|eventfd_write|eventfd|timerfd_settime|timerfd_gettime|timerfd_create|arch_prctl|procctl|prctl|ptrace|ktrace|tcgetattr|tcsetattr|cfmakeraw|getpagesizes|getpagesize|lpathconf|sysconf|fpathconf|pathconf|getrusage|nftw|ftw|wordexp|wordfree|getloginclass|setloginclass|login_getclass|setusercontext|setlogin|getlogin_r|getlogin|ttyname_r|ttyname|inet_pton|inet_ntop|inet_aton|munlockall|mlockall|munlock|mlock2|mseal|mlock|posix_fadvise64|posix_fadvise|readahead|posix_madvise|madvise|vmsplice|splice|inotify_init1|inotify_init|inotify_add_watch|inotify_rm_watch|fdatasync|fsync|arc4random_uniform|arc4random_buf|arc4random|getrandom|getentropy|issetugid|getdelim|getline|vasprintf|asprintf|strlcpy|strlcat|timingsafe_memcmp|timingsafe_bcmp|explicit_bzero|memset_s|explicit_memset|isatty|posix_openpt|ptsname_r|ptsname|grantpt|unlockpt|unmount|nmount|umount2|umount|mount|open_wmemstream|open_memstream|fmemopen|scandir|setxattrat|getxattrat|listxattrat|removexattrat|extattr_set_file|extattr_get_file|extattr_delete_file|extattr_list_file|extattr_set_fd|extattr_get_fd|extattr_delete" "_fd|extattr_list_fd|extattr_set_link|extattr_get_link|extattr_delete_link|extattr_list_link|lsetxattr|fsetxattr|setxattr|listxattr|removexattr|getxattr|pthread_yield|sched_yield|sched_setattr|sched_getattr|cpuset_setaffinity|cpuset_getaffinity|sched_get_priority_max|sched_get_priority_min|sched_setaffinity|sched_getaffinity|sched_setscheduler|sched_getscheduler|sched_setparam|sched_getparam|lio_listio|aio_suspend|aio_return|aio_error|aio_write|aio_read|io_uring_register|io_uring_setup|io_uring_enter|capset|capget|statx|pidfd_send_signal|pidfd_getfd|pidfd_open|fanotify_mark|fanotify_init|seccomp|getgrnam|getgrgid|getspnam|posix_fallocate|fallocate|close_range|closefrom|lsm_get_self_attr|lsm_set_self_attr|lsm_list_modules|landlock_create_ruleset|landlock_add_rule|landlock_restrict_self|rtprio_thread|rtprio|getpriority|setpriority|signalfd|sigaction|pthread_sigmask|sigprocmask|sigsuspend|sigaltstack|sigwaitinfo|sigtimedwait|sigpending|sigwait|bpf|userfaultfd|getpass|getgrouplist|getgroups|initgroups|setgroups|unshare|setns|clone|openat2|sendmmsg|recvmmsg|name_to_handle_at|open_by_handle_at|process_madvise|personality|quotactl|pivot_root|membarrier|pkey_alloc|getmntinfo|getvfsbyname|getfsent|setfsent|endfsent|getfsstat|statfs|syncfs|prlimit64|prlimit|migrate_pages|move_pages|process_vm_readv|perf_event_open|clone3|kcmp|keyctl|open_tree_attr|fsopen|fsmount|open_tree|move_mount|fspic" "k|fsconfig|process_mrelease|memfd_secret|ioprio_set|ioprio_get|mq_open|shmget|_umtx_op|futex_waitv|futex_wake|futex_wait|futex_requeue|futex|ntp_adjtime|ntp_gettime|adjtimex|adjtime|revoke|fflagstostr|strtofflags|strmode|strtonum|reallocarray|reallocf|getprogname|setprogname|setproctitle|daemon|auditon|getaudit|setaudit|auditctl|kinfo_getproc|kinfo_getfile|kinfo_getvmmap|kvm_openfiles|kvm_open|kvm_getprocs|kvm_close|kvm_nlist|uuidgen|setfib|kenv|mac_set_proc|mac_get_proc|mac_set_fd|mac_get_fd|mac_set_file|mac_get_file|sethostname|reboot|swapon|swapoff|acct|ioperm|iopl|mincore|rseq|timer_create|semget|msgget|syslog|klogctl|mount_setattr|getcpu|init_module|finit_module|delete_module|kexec_load|kexec_file_load|quotactl_fd|pkey_free|pkey_mprotect|tgkill|add_key|semctl|msgctl|shmctl|timer_settime|getdomainname|setdomainname|io_submit|io_getevents|io_setup|io_destroy|io_cancel|io_pgetevents|request_key|tkill|timer_delete|timer_gettime|timer_getoverrun|mq_unlink|mq_timedsend|mq_timedreceive|mq_notify|mq_getsetattr|shmat|shmdt|semop|semtimedop|msgsnd|msgrcv|sync_file_range|remap_file_pages|msync|mremap|socketpair|sysinfo|gettid|setitimer|getitimer|nice|getdirentries|getdents64|getdents|utimensat|futimens|utimes|linkat|set_mempolicy_home_node|mbind|set_mempolicy|get_mempolicy|cachestat|map_shadow_stack|throw_with_nested|rethrow_if_nested|__atomic_load|__atomic_store|__sync_fetch_and_add" "|__sync_bool_compare_and_swap|wcscpy|wcscat|wcsncpy|wcsncat)\\s*\\(", body)) {
        return std::format("libc buffer/tempfile unencoded: unconstrained call is not a proof of the buffer ({})", engine);
    }
    if (prism::re_search("(?:,|\\()\\s*&", body)) {
        return std::format("address-of unencoded: {} is not a pointer-value model", engine);
    }
    if (prism::re_search("\\b(?:char|unsigned\\s+char)\\s+[A-Za-z_]\\w*\\s*\\[\\s*\\]\\s*=", body)) {
        return std::format("array string-init unencoded: {} is not a string-array model", engine);
    }
    if (prism::re_search("\\b(?:asm|__asm__|__asm)\\b", body)) {
        return std::format("inline asm unencoded: {} is not an assembly model", engine);
    }
    if (prism::re_search("\\b_Generic\\b", body)) {
        return std::format("_Generic unencoded: {} is not a type-generic model", engine);
    }
    if (prism::re_search("\\(\\s*\\{", body)) {
        return std::format("GNU statement expression unencoded: {} is not a GNU-C model", engine);
    }
    if (prism::re_search("\\b(?:try|catch)\\b", body)) {
        return std::format("C++ try/catch unencoded: {} is not an exception model", engine);
    }
    if (prism::re_search("\\boffsetof\\b", body)) {
        return std::format("offsetof unencoded: {} is not a struct-layout model", engine);
    }
    if (prism::re_search("\\b(?:new|delete)\\b", body)) {
        return std::format("C++ new/delete unencoded: {} is not a heap-lifetime model", engine);
    }
    if (prism::re_search("\\brestrict\\b", body)) {
        return std::format("restrict unencoded: {} is not a restrict-qualifier model", engine);
    }
    if (prism::re_search("(?m)(?:^|[;{(])\\s*(?:(?:static|extern|auto|register|const)\\s+)*(?:_Atomic|volatile)\\b|\\b_Atomic\\s*\\(|\\b(?:int|unsigned|signed|long|short|char|uint\\w*|int\\w*|_Bool|bool|void|float|double)\\s+(?:const\\s+)?volatile\\b", body)) {
        return std::format("volatile/_Atomic unencoded: {} is not a memory-model", engine);
    }
    if (prism::re_search("\\b(?:pthread_mutex_t|mtx_t)\\b", body)) {
        return std::format("mutex object unencoded: {} is not a lock model", engine);
    }
    if (prism::re_search("(?m)(?:^|[;{])\\s*(?:(?:static|extern|auto|register)\\s+)*const\\s+|\\b(?:int|unsigned|signed|long|short|char|uint\\w*|int\\w*|_Bool|bool|void|float|double|size_t)\\s+const\\s+[A-Za-z_]|\\bfor\\s*\\(\\s*(?:(?:static|extern|auto|register)\\s+)*const\\s+", body)) {
        return std::format("const local unencoded: {} is not a const model", engine);
    }
    if (prism::re_search("(?m)(?:^|[;{(])\\s*(?:register|auto)\\b", body)) {
        return std::format("register/auto unencoded: {} is not a storage-class or auto-type model", engine);
    }
    if (prism::re_search("\\b__auto_type\\b", body)) {
        return std::format("__auto_type unencoded: {} is not a storage-class or auto-type model", engine);
    }
    if (prism::re_search("(?m)(?:^|[;{])\\s*(?:static|extern)\\s+(?:unsigned\\s+long\\s+long(?:\\s+int)?|long\\s+long(?:\\s+int)?|unsigned\\s+long(?:\\s+int)?|uint64_t|int64_t|unsigned(?:\\s+int)?|int|long|short|char|uint32_t|int32_t|size_t)\\s+[A-Za-z_]\\w*", body)) {
        return std::format("static/extern local unencoded: {} is not a storage-duration model", engine);
    }
    if (prism::re_search("enum\\s*\\{[^}]+\\}\\s+[A-Za-z_]\\w*", body)) {
        return std::format("anonymous enum local unencoded: {} is not a layout model", engine);
    }
    if (prism::re_search("(?m)(?:^|[;{])\\s*(?:_Alignas|alignas)\\s*\\(", body)) {
        return std::format("_Alignas unencoded: {} is not an alignment model", engine);
    }
    if (prism::re_search("(?m)(?:^|[;{])\\s*(?:(?:static|extern|auto|register|const)\\s+)*(?:unsigned\\s+long\\s+long(?:\\s+int)?|long\\s+long(?:\\s+int)?|unsigned\\s+long(?:\\s+int)?|uint64_t|int64_t|unsigned(?:\\s+int)?|int|long|short|char|uint32_t|int32_t|size_t)\\s+[A-Za-z_]\\w*\\s*=\\s*\\([^)]*\\)\\s*\\{", body)) {
        return std::format("compound literal unencoded: {} is not a compound-literal model", engine);
    }
    if (prism::re_search("(?m)(?:^|[;{])\\s*(?:(?:static|extern|auto|register|const)\\s+)*(?:struct|union)(?:\\s+[A-Za-z_]\\w*)?\\s*\\{|(?:^|[;{])\\s*(?:(?:static|extern|auto|register|const)\\s+)*(?:struct|union|enum)\\s+[A-Za-z_]\\w*\\s+[A-Za-z_]\\w*", body)) {
        return std::format("struct/union local unencoded: {} is not a layout model", engine);
    }
    {
        static const std::unordered_set<std::string> start{
            "if", "for", "while", "switch", "return", "sizeof", "typeof", "else", "do", "case", "default", "goto", "break", "continue", "assert", "throw", "try", "catch", "asm", "__asm__", "__asm", "typedef", "static", "extern", "auto", "register", "int", "unsigned", "signed", "long", "short", "char", "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t", "void", "float", "double", "_Bool", "bool", "const", "volatile", "_Atomic", "struct", "union", "enum"};
        static prism::Regex td("(?m)(?:^|[;{])\\s*(?:(?:static|extern|auto|register)\\s+)*([A-Za-z_]\\w*)\\s+[A-Za-z_]\\w*\\s*(?:[=;\\[])", true);
        for (auto& m : td.finditer(body)) {
            if (!start.contains(m.group(1)))
                return std::format("unknown typedef local unencoded: {} is not a layout model", engine);
        }
    }
    return std::nullopt;
}
