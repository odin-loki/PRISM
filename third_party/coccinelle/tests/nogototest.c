int main () {
  if (x) {
     a();
     goto out;
  }
  b();
out:
  return;
}
