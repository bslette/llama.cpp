set print pretty on
set print repeats 4
set debuginfod enabled off
break psdump.c:17
run
p *top
p *sl
p *me
p *md
p *q
