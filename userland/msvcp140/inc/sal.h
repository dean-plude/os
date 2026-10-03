/*
 * NovaOS: the SAL annotations the STL uses, over MinGW-w64's <sal.h>
 * (which lacks some); annotations only guide static analysis.
 */
#pragma once
#include_next <sal.h>
#ifndef _Check_return_
#define _Check_return_
#endif
#ifndef _Check_return_opt_
#define _Check_return_opt_
#endif
#ifndef _Deref_post_opt_valid_
#define _Deref_post_opt_valid_
#endif
#ifndef _Field_z_
#define _Field_z_
#endif
#ifndef _In_
#define _In_
#endif
#ifndef _In_NLS_string_
#define _In_NLS_string_(...)
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _In_opt_z_
#define _In_opt_z_
#endif
#ifndef _In_z_
#define _In_z_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Outptr_opt_result_maybenull_
#define _Outptr_opt_result_maybenull_
#endif
#ifndef _Reserved_
#define _Reserved_
#endif
#ifndef _Ret_z_
#define _Ret_z_
#endif
#ifndef _Analysis_assume_
#define _Analysis_assume_(...)
#endif
#ifndef _Analysis_assume_lock_held_
#define _Analysis_assume_lock_held_(...)
#endif
#ifndef _Guarded_by_
#define _Guarded_by_(...)
#endif
#ifndef _In_range_
#define _In_range_(...)
#endif
#ifndef _In_reads_
#define _In_reads_(...)
#endif
#ifndef _In_reads_bytes_
#define _In_reads_bytes_(...)
#endif
#ifndef _Inout_bytecount_
#define _Inout_bytecount_(...)
#endif
#ifndef _Out_writes_
#define _Out_writes_(...)
#endif
#ifndef _Out_writes_all_
#define _Out_writes_all_(...)
#endif
#ifndef _Out_writes_bytes_
#define _Out_writes_bytes_(...)
#endif
#ifndef _Out_writes_opt_
#define _Out_writes_opt_(...)
#endif
#ifndef _Out_writes_to_opt_
#define _Out_writes_to_opt_(...)
#endif
#ifndef _Out_writes_z_
#define _Out_writes_z_(...)
#endif
#ifndef _Post_equal_to_
#define _Post_equal_to_(...)
#endif
#ifndef _Post_readable_size_
#define _Post_readable_size_(...)
#endif
#ifndef _Post_satisfies_
#define _Post_satisfies_(...)
#endif
#ifndef _Pre_satisfies_
#define _Pre_satisfies_(...)
#endif
#ifndef _Ret_range_
#define _Ret_range_(...)
#endif
#ifndef _Success_
#define _Success_(...)
#endif
#ifndef _When_
#define _When_(...)
#endif
#ifndef _Acquires_lock_
#define _Acquires_lock_(...)
#endif
#ifndef _Releases_lock_
#define _Releases_lock_(...)
#endif
#ifndef _Requires_lock_held_
#define _Requires_lock_held_(...)
#endif
#ifndef _Acquires_exclusive_lock_
#define _Acquires_exclusive_lock_(...)
#endif
#ifndef _Releases_exclusive_lock_
#define _Releases_exclusive_lock_(...)
#endif
#ifndef _Acquires_shared_lock_
#define _Acquires_shared_lock_(...)
#endif
#ifndef _Releases_shared_lock_
#define _Releases_shared_lock_(...)
#endif
#ifndef _Requires_lock_not_held_
#define _Requires_lock_not_held_(...)
#endif
#ifndef _Ret_notnull_
#define _Ret_notnull_(...)
#endif
#ifndef _Printf_format_string_params_
#define _Printf_format_string_params_(...)
#endif
#ifndef _Scanf_format_string_params_
#define _Scanf_format_string_params_(...)
#endif
#ifndef _In_reads_z_
#define _In_reads_z_(...)
#endif
#ifndef _In_reads_opt_
#define _In_reads_opt_(...)
#endif
#ifndef _In_reads_or_z_
#define _In_reads_or_z_(...)
#endif
#ifndef _Out_writes_bytes_all_
#define _Out_writes_bytes_all_(...)
#endif
#ifndef _Out_writes_bytes_opt_
#define _Out_writes_bytes_opt_(...)
#endif
#ifndef _Out_writes_bytes_to_
#define _Out_writes_bytes_to_(...)
#endif
#ifndef _Out_writes_to_
#define _Out_writes_to_(...)
#endif
#ifndef _Outptr_result_buffer_
#define _Outptr_result_buffer_(...)
#endif
#ifndef _Pre_notnull_
#define _Pre_notnull_(...)
#endif
