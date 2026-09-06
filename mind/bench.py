import sys

# Increase the stack limit to handle deep recursion
sys.setrecursionlimit(2500000)

def count_down(n):
    if n <= 0:
        return 0
    else:
        return count_down(n - 1)

def sum_to(n):
    if n <= 0:
        return 0
    else:
        return n + sum_to(n - 1)

def fac(n):
    if n <= 1:
        return 1
    else:
        return n * fac(n - 1)

def main():
    print(f"tail: {count_down(2000000)}")
    print(f"non-tail: {sum_to(100000)}")
    print(f"fac 20: {fac(20)}")

if __name__ == "__main__":
    main()