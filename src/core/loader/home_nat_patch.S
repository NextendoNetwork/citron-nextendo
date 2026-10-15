.text
.global nat_begin
.extern getter
.extern set_text
nat_begin:
 stp x29,x30,[sp,#-64]!
 mov x29,sp
 add x0,sp,#16
 add x1,sp,#24
 mov x2,#4
 adr x3,category
 adr x4,begin_name
 bl getter
 ldp x29,x30,[sp],#64
 mov w8,#0x126c
 ret
nat_complete:
 stp x29,x30,[sp,#-64]!
 mov x29,sp
 str w0,[sp,#32]
 cbnz w0,complete_done
 ldr w8,[x19,#0x126c]
 cmp w8,#4
 b.hi complete_done
 adr x4,save_names
 add x4,x4,x8,lsl #4
 adr x3,category
 add x0,sp,#16
 add x1,sp,#24
 mov x2,#4
 bl getter
complete_done:
 ldr w0,[sp,#32]
 ldp x29,x30,[sp],#64
 ldr x19,[sp,#16]
 ret
nat_status:
 stp x29,x30,[sp,#-192]!
 mov x29,sp
 add x0,sp,#16
 add x1,sp,#48
 mov x2,#128
 adr x3,category
 adr x4,status_name
 bl getter
 cbnz w0,status_done
 str x21,[sp,#24]
 add x8,sp,#48
 str x8,[sp,#32]
 add x0,sp,#24
 add x1,sp,#32
 bl set_text
status_done:
 ldp x29,x30,[sp],#192
 ldp x20,x19,[sp,#48]
 ret
category: .asciz "citron_netdiag"
begin_name: .asciz "begin"
status_name: .asciz "status_text"
.balign 16
save_names:
 .asciz "save_nat_0"
 .balign 16
 .asciz "save_nat_1"
 .balign 16
 .asciz "save_nat_2"
 .balign 16
 .asciz "save_nat_3"
 .balign 16
 .asciz "save_nat_4"
