#include <stdio.h>
int add(int a, int b) { return a + b; }
int main(void) {
    int total = 0;
    for (int i = 1; i <= 3; i++) {
        total = add(total, i);
        printf("total = %d\n", total);
    }
    return 0;
}
