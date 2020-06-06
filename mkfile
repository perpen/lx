all:V: both.all
install:V: both.install

both.%:V: clt.% srv.%

clt.%:V:
	mk -f mkfile.clt $stem
srv.%:V:
	lx mk -f mkfile.srv $stem

watch:V:
	watch *.[ch] -- mk install

gitpush:V:
	git/commit README acid mkfile* sigtest *.[ch] \
		bin/* man/lx.^(1 6) \
		`{ls lx-dwm | grep -v '/(config.h|.*\.o|lx-dwm)$'}
	git/push

# merges beta into master
gitmerge:V:
	b=`{git/branch}
	git/branch beta
	git/pull
	git/branch master
	git/merge beta
	git/push
	git/branch $b
