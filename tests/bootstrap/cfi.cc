struct Guard {
    ~Guard();
};
void may_throw();

void needs_unwind() {
    Guard g;
    may_throw();
}
