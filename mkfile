usage:VQ:
	echo 'usage: mk clt.TARGET or srv.TARGET or both.TARGET'
	echo '	e.g. to install the client: "mk clt.install"'
	echo 'This mkfile is meant to be used from Plan 9, not Linux'
	echo 'The srv.* targets require a working lx installation.'

#both.%:V: clt.% srv.%
both.%:V:
	mk -f mkfile.clt $stem
	lx mk -f mkfile.srv $stem

clt.%:V:
	mk -f mkfile.clt $stem

srv.%:V:
	lx mk -f mkfile.srv $stem

#### The targets below are helpers used during development

# Run install on change
watch:V:
	while() {
		newsum=`{ls -q mkfile* *.[ch] | sum | awk '{print $1}'}
		if(! ~ $newsum $oldsum) {
			oldsum=$newsum
			echo '########' `{date}
			mk both.install || echo oops
		}
		sleep 2
	}

# Restart the server on build, see mkfile.srv:/pkill
# This assumes the current name is not 'lx'
srvloop:V:
	# FIXME weirdly 9pfuse fails when lx2srv is started from here
	exe=`{cat name}^srv
	echo $exe
	lx pkill $exe || echo -n
	while(){ echo '####' `{date}; lx $exe -p 8000 -i 192.168.0.2 || sleep 3 }

gitpush:V:
	# Do not commit ./name as its content must stay 'lx' in the
	# master branch.
	git/commit `{git/walk | awk '{print $2}' | grep -v '^name$'}
	git/push

gitmerge:V:
	git/branch master
	echo lx > name
	git/merge beta
	echo lx > name
	git/push
	git/branch beta
	echo lx2 > name
