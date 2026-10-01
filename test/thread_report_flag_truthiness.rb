# Thread.report_on_exception= and Thread#report_on_exception= take their
# value's truthiness, whatever its class, as CRuby's flag does. A boxed value
# went into the runtime's boolean parameter as it was, and the C did not
# build.

@r = Thread.report_on_exception
Thread.report_on_exception = @r
p Thread.report_on_exception
Thread.report_on_exception = [nil, 1][0]
p Thread.report_on_exception
Thread.report_on_exception = 0
p Thread.report_on_exception
t = Thread.new { }
t.report_on_exception = [false, 1][0]
p t.report_on_exception
t.report_on_exception = "yes"
p t.report_on_exception
t.join
