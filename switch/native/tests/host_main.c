int native_test_double_mode(void);
int native_test_fp_state(void);
int native_test_xfk(void);
int main(void) { return native_test_double_mode() | native_test_fp_state() | native_test_xfk(); }
