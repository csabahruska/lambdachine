# removes trailing spaces
find . -name "*.cabal" -type f -print0 | xargs -0 sed -i 's/[[:space:]]*$//'
find . -name "*.hs" -type f -print0 | xargs -0 sed -i 's/[[:space:]]*$//'
find . -name "*.yaml" -type f -print0 | xargs -0 sed -i 's/[[:space:]]*$//'
find . -name "*.h" -type f -print0 | xargs -0 sed -i 's/[[:space:]]*$//'
find . -name "*.hh" -type f -print0 | xargs -0 sed -i 's/[[:space:]]*$//'
find . -name "*.c" -type f -print0 | xargs -0 sed -i 's/[[:space:]]*$//'
find . -name "*.cc" -type f -print0 | xargs -0 sed -i 's/[[:space:]]*$//'

# expand tabs to spaces
find . -name '*.hs' ! -type d -exec bash -c 'expand "$0" > /tmp/e && mv /tmp/e "$0"' {} \;
find . -name '*.h' ! -type d -exec bash -c 'expand "$0" > /tmp/e && mv /tmp/e "$0"' {} \;
find . -name '*.hh' ! -type d -exec bash -c 'expand "$0" > /tmp/e && mv /tmp/e "$0"' {} \;
find . -name '*.c' ! -type d -exec bash -c 'expand "$0" > /tmp/e && mv /tmp/e "$0"' {} \;
find . -name '*.cc' ! -type d -exec bash -c 'expand "$0" > /tmp/e && mv /tmp/e "$0"' {} \;
