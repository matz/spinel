# frozen_string_literal: true

class Date
  include Comparable

  Error = Class.new(ArgumentError)

  # ---------------------------------------------------------------------------
  # Class methods
  # ---------------------------------------------------------------------------

  class << self
    # Same as Date.new.
    def civil(year = -4712, month = 1, day = 1, start = ITALY)
      if Integer === year && Integer === month && Integer === day &&
         month >= 1 && month <= 12 && day >= 1 && day <= 28
        # Days 1..28 exist in every month of both the Julian and Gregorian
        # calendars, so such a date can only be invalid if it falls in the gap
        # skipped at the calendar reform (gregorian jd < start <= julian jd).
        # Inline civil_to_jd so the reform gap can be detected without a second
        # pass: keep both the Gregorian and Julian Julian Day numbers.
        yy = month <= 2 ? year - 1 : year
        gjd_base = (1461 * (yy + 4716)) / 4 + GJD_MONTH_OFFSET[month] + day
        a = yy / 100
        gjd = gjd_base - 1524 + 2 - a + a / 4
        if gjd >= start
          # month/day are in the 1..12 / 1..28 fast-path range, so year, month
          # and day are already the final civil fields; cache them directly.
          return new_from_jd(gjd, start, year, month, day)
        else
          jjd = gjd_base - 1524
          return new_from_jd(jjd, start, year, month, day) if jjd < start
          # gjd < start <= jjd: date falls in the reform gap; fall through to
          # civil_fallback, which raises "invalid date".
        end
      end
      civil_fallback(year, month, day, start)
    end

    # call-seq:
    #   Date.new(year = -4712, month = 1, mday = 1, start = Date::ITALY) -> date
    #
    # Returns a new \Date object constructed from the given arguments:
    #
    #   Date.new(2022).to_s        # => "2022-01-01"
    #   Date.new(2022, 2).to_s     # => "2022-02-01"
    #   Date.new(2022, 2, 4).to_s  # => "2022-02-04"
    #
    # Argument +month+ should be in range (1..12) or range (-12..-1);
    # when the argument is negative, counts backward from the end of the year:
    #
    #   Date.new(2022, -11, 4).to_s # => "2022-02-04"
    #
    # Argument +mday+ should be in range (1..n) or range (-n..-1)
    # where +n+ is the number of days in the month;
    # when the argument is negative, counts backward from the end of the month.
    #
    # See argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    #
    # Related: Date.jd.
    alias_method :new, :civil

    # call-seq:
    #   Date.valid_civil?(year, month, mday, start = Date::ITALY) -> true or false
    #
    # Returns +true+ if the arguments define a valid ordinal date,
    # +false+ otherwise:
    #
    #   Date.valid_date?(2001, 2, 3)  # => true
    #   Date.valid_date?(2001, 2, 29) # => false
    #   Date.valid_date?(2001, 2, -1) # => true
    #
    # See argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    #
    # Related: Date.jd, Date.new.
    def valid_civil?(year, month, day, start = ITALY)
      return false unless year.is_a?(Numeric) && month.is_a?(Numeric) && day.is_a?(Numeric)
      !!internal_valid_civil?(year, month, day, start)
    end
    alias_method :valid_date?, :valid_civil?

    # call-seq:
    #   Date.jd(jd = 0, start = Date::ITALY) -> date
    #
    # Returns a new \Date object formed from the arguments:
    #
    #   Date.jd(2451944).to_s # => "2001-02-03"
    #   Date.jd(2451945).to_s # => "2001-02-04"
    #   Date.jd(0).to_s       # => "-4712-01-01"
    #
    # The returned date is:
    #
    # - Gregorian, if the argument is greater than or equal to +start+:
    #
    #     Date::ITALY                         # => 2299161
    #     Date.jd(Date::ITALY).gregorian?     # => true
    #     Date.jd(Date::ITALY + 1).gregorian? # => true
    #
    # - Julian, otherwise
    #
    #     Date.jd(Date::ITALY - 1).julian?    # => true
    #
    # See argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    #
    # Related: Date.new.
    def jd(jd = 0, start = ITALY)
      jd = convert_numeric_to_int(jd, "jd")
      obj = allocate
      obj.__send__(:initialize, jd, start)
      obj
    end

    # call-seq:
    #   Date.valid_jd?(jd, start = Date::ITALY) -> true
    #
    # Implemented for compatibility;
    # returns +true+ unless +jd+ is invalid (i.e., not a Numeric).
    #
    #   Date.valid_jd?(2451944) # => true
    #
    # See argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    #
    # Related: Date.jd.
    def valid_jd?(jd, _start = ITALY)
      jd.is_a?(Numeric)
    end

    # call-seq:
    #   Date.gregorian_leap?(year) -> true or false
    #
    # Returns +true+ if the given year is a leap year
    # in the {proleptic Gregorian calendar}[https://en.wikipedia.org/wiki/Proleptic_Gregorian_calendar], +false+ otherwise:
    #
    #   Date.gregorian_leap?(2000) # => true
    #   Date.gregorian_leap?(2001) # => false
    #
    # Related: Date.julian_leap?.
    def gregorian_leap?(year)
      raise TypeError, "expected numeric" unless year.is_a?(Numeric)
      internal_gregorian_leap?(year)
    end
    alias_method :leap?, :gregorian_leap?

    # call-seq:
    #   Date.julian_leap?(year) -> true or false
    #
    # Returns +true+ if the given year is a leap year
    # in the {proleptic Julian calendar}[https://en.wikipedia.org/wiki/Proleptic_Julian_calendar], +false+ otherwise:
    #
    #   Date.julian_leap?(1900) # => true
    #   Date.julian_leap?(1901) # => false
    #
    # Related: Date.gregorian_leap?.
    def julian_leap?(year)
      raise TypeError, "expected numeric" unless year.is_a?(Numeric)
      internal_julian_leap?(year)
    end

    # call-seq:
    #   Date.ordinal(year = -4712, yday = 1, start = Date::ITALY) -> date
    #
    # Returns a new \Date object formed fom the arguments.
    #
    # With no arguments, returns the date for January 1, -4712:
    #
    #   Date.ordinal.to_s # => "-4712-01-01"
    #
    # With argument +year+, returns the date for January 1 of that year:
    #
    #   Date.ordinal(2001).to_s  # => "2001-01-01"
    #   Date.ordinal(-2001).to_s # => "-2001-01-01"
    #
    # With positive argument +yday+ == +n+,
    # returns the date for the +nth+ day of the given year:
    #
    #   Date.ordinal(2001, 14).to_s # => "2001-01-14"
    #
    # With negative argument +yday+, counts backward from the end of the year:
    #
    #   Date.ordinal(2001, -14).to_s # => "2001-12-18"
    #
    # Raises an exception if +yday+ is zero or out of range.
    #
    # See argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    #
    # Related: Date.jd, Date.new.
    def ordinal(year = -4712, yday = 1, start = ITALY)
      if Integer === year && Integer === yday && yday >= 1 && yday <= 365
        jd1 = civil_to_jd(year, 1, 1, start)
        jd = jd1 + yday - 1
        # Guard against the shortened reform year (fewer than 365 days), where
        # some ydays <= 365 do not exist: the day must still precede next Jan 1.
        if jd < civil_to_jd(year + 1, 1, 1, start)
          return new_from_jd(jd, start)
        end
      end
      yday = convert_numeric_to_int(yday, "yday")
      year = convert_numeric_to_int(year, "year")
      jd = internal_valid_ordinal?(year, yday, start)
      raise Date::Error, "invalid date" unless jd
      new_from_jd(jd, start)
    end

    # call-seq:
    #   Date.valid_ordinal?(year, yday, start = Date::ITALY) -> true or false
    #
    # Returns +true+ if the arguments define a valid ordinal date,
    # +false+ otherwise:
    #
    #   Date.valid_ordinal?(2001, 34)  # => true
    #   Date.valid_ordinal?(2001, 366) # => false
    #
    # See argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    #
    # Related: Date.jd, Date.ordinal.
    def valid_ordinal?(year, day, start = ITALY)
      return false unless year.is_a?(Numeric) && day.is_a?(Numeric)
      !!internal_valid_ordinal?(year, day, start)
    end

    # call-seq:
    #   Date.commercial(cwyear = -4712, cweek = 1, cwday = 1, start = Date::ITALY) -> date
    #
    # Returns a new \Date object constructed from the arguments.
    #
    # Argument +cwyear+ gives the year, and should be an integer.
    #
    # Argument +cweek+ gives the index of the week within the year,
    # and should be in range (1..53) or (-53..-1);
    # in some years, 53 or -53 will be out-of-range;
    # if negative, counts backward from the end of the year:
    #
    #   Date.commercial(2022, 1, 1).to_s  # => "2022-01-03"
    #   Date.commercial(2022, 52, 1).to_s # => "2022-12-26"
    #
    # Argument +cwday+ gives the indes of the weekday within the week,
    # and should be in range (1..7) or (-7..-1);
    # 1 or -7 is Monday;
    # if negative, counts backward from the end of the week:
    #
    #   Date.commercial(2022, 1, 1).to_s  # => "2022-01-03"
    #   Date.commercial(2022, 1, -7).to_s # => "2022-01-03"
    #
    # When +cweek+ is 1:
    #
    # - If January 1 is a Friday, Saturday, or Sunday,
    #   the first week begins in the week after:
    #
    #     Date::ABBR_DAYNAMES[Date.new(2023, 1, 1).wday] # => "Sun"
    #     Date.commercial(2023, 1, 1).to_s # => "2023-01-02"
    #     Date.commercial(2023, 1, 7).to_s # => "2023-01-08"
    #
    # - Otherwise, the first week is the week of January 1,
    #   which may mean some of the days fall on the year before:
    #
    #     Date::ABBR_DAYNAMES[Date.new(2020, 1, 1).wday] # => "Wed"
    #     Date.commercial(2020, 1, 1).to_s # => "2019-12-30"
    #     Date.commercial(2020, 1, 7).to_s # => "2020-01-05"
    #
    # See argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    #
    # Related: Date.jd, Date.new, Date.ordinal.
    def commercial(cwyear = -4712, cweek = 1, cwday = 1, start = ITALY)
      if Integer === cwyear && Integer === cweek && Integer === cwday &&
         cweek >= 1 && cweek <= 52 && cwday >= 1 && cwday <= 7
        # ISO 8601: every normal year has at least 52 weeks, but the
        # calendar-reform year is shorter; confirm the day precedes the start of
        # the next commercial year before trusting the fast path.
        jd = commercial_to_jd(cwyear, cweek, cwday, start)
        return new_from_jd(jd, start) if jd < commercial_to_jd(cwyear + 1, 1, 1, start)
      end
      cwday  = convert_numeric_to_int(cwday, "cwday")
      cweek  = convert_numeric_to_int(cweek, "cweek")
      cwyear = convert_numeric_to_int(cwyear, "year")
      jd = internal_valid_commercial?(cwyear, cweek, cwday, start)
      raise Date::Error, "invalid date" unless jd
      new_from_jd(jd, start)
    end

    # call-seq:
    #   Date.valid_commercial?(cwyear, cweek, cwday, start = Date::ITALY) -> true or false
    #
    # Returns +true+ if the arguments define a valid commercial date,
    # +false+ otherwise:
    #
    #   Date.valid_commercial?(2001, 5, 6) # => true
    #   Date.valid_commercial?(2001, 5, 8) # => false
    #
    # See Date.commercial.
    #
    # See argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    #
    # Related: Date.jd, Date.commercial.
    def valid_commercial?(year, week, day, start = ITALY)
      return false unless year.is_a?(Numeric) && week.is_a?(Numeric) && day.is_a?(Numeric)
      !!internal_valid_commercial?(year, week, day, start)
    end

    # call-seq:
    #   Date.today(start = Date::ITALY) -> date
    #
    # Returns a new \Date object constructed from the present date:
    #
    #   Date.today.to_s # => "2022-07-06"
    #
    # See argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    #
    def today(start = ITALY)
      t = Time.now
      jd = civil_to_jd(t.year, t.mon, t.mday, start)
      new_from_jd(jd, start)
    end

    # :nodoc:
    def _load(s)
      a = Marshal.load(s)
      obj = allocate
      obj.marshal_load(a)
      obj
    end

    private

    # Convert a Numeric argument to an Integer. Rejects non-Numeric input up
    # front: a bare Integer() conversion would coerce numeric-looking strings
    # such as "0xa", so require a Numeric here and raise TypeError otherwise.
    def convert_numeric_to_int(obj, field)
      unless Numeric === obj
        raise TypeError, "invalid #{field} (not numeric)"
      end
      Integer(obj)
    end

    def civil_fallback(year, month, day, start)
      day   = convert_numeric_to_int(day, "day")
      month = convert_numeric_to_int(month, "month")
      year  = convert_numeric_to_int(year, "year")
      jd = internal_valid_civil?(year, month, day, start)
      raise Date::Error, "invalid date" unless jd
      new_from_jd(jd, start)
    end

    # ---------------------------------------------------------------------------
    # Internal calendar arithmetic (pure Ruby, no C dependency)
    # ---------------------------------------------------------------------------

    # Gregorian leap year?
    def internal_gregorian_leap?(y)
      (y % 4 == 0 && y % 100 != 0) || y % 400 == 0
    end

    # Julian leap year?
    def internal_julian_leap?(y)
      y % 4 == 0
    end

    def days_in_month_gregorian(y, m)
      if m == 2 && internal_gregorian_leap?(y)
        29
      else
        DAYS_IN_MONTH_GREGORIAN[m]
      end
    end

    # Days in month for Julian calendar.
    def days_in_month_julian(y, m)
      if m == 2 && internal_julian_leap?(y)
        29
      else
        DAYS_IN_MONTH_GREGORIAN[m]
      end
    end

    # Gregorian civil (year, month, day) -> Julian Day Number
    def gregorian_to_jd(y, m, d)
      if m <= 2
        y -= 1
        m += 12
      end
      a = y / 100
      b = 2 - a + a / 4
      (1461 * (y + 4716)) / 4 + (306001 * (m + 1)) / 10000 + d + b - 1524
    end

    # Julian civil (year, month, day) -> Julian Day Number
    def julian_to_jd(y, m, d)
      if m <= 2
        y -= 1
        m += 12
      end
      (1461 * (y + 4716)) / 4 + (306001 * (m + 1)) / 10000 + d - 1524
    end

    # Civil (year, month, day) -> JD, respecting start (cutover).
    # Uses a unified formula: gjd >= sg -> Gregorian JD, else Julian JD.
    # Works for all sg values: Float::INFINITY (always Julian),
    # -Float::INFINITY (always Gregorian), or integer cutover JD.
    def civil_to_jd(y, m, d, sg)
      offset = GJD_MONTH_OFFSET[m]
      y -= 1 if m <= 2
      gjd_base = (1461 * (y + 4716)) / 4 + offset + d
      a = y / 100
      gjd = gjd_base - 1524 + 2 - a + a / 4
      gjd >= sg ? gjd : gjd_base - 1524
    end

    # Gregorian JD -> (year, month, day)
    def jd_to_gregorian(jd)
      a = jd + 32044
      b = (4 * a + 3).div(146097)
      c = a - (146097 * b).div(4)
      d = (4 * c + 3).div(1461)
      e = c - (1461 * d).div(4)
      m = (5 * e + 2).div(153)
      day  = e - (153 * m + 2).div(5) + 1
      mon  = m + 3 - 12 * m.div(10)
      year = 100 * b + d - 4800 + m.div(10)
      [year, mon, day]
    end

    # Julian JD -> (year, month, day)
    def jd_to_julian(jd)
      c = jd + 32082
      d = (4 * c + 3).div(1461)
      e = c - (1461 * d).div(4)
      m = (5 * e + 2).div(153)
      day  = e - (153 * m + 2).div(5) + 1
      mon  = m + 3 - 12 * m.div(10)
      year = d - 4800 + m.div(10)
      [year, mon, day]
    end

    # JD -> (year, month, day), respecting start (cutover).
    def jd_to_civil(jd, sg)
      if sg == Float::INFINITY
        jd_to_julian(jd)
      elsif sg == -Float::INFINITY || jd >= sg
        jd_to_gregorian(jd)
      else
        jd_to_julian(jd)
      end
    end

    # Ordinal (year, day-of-year) -> JD
    def ordinal_to_jd(y, d, sg)
      civil_to_jd(y, 1, 1, sg) + d - 1
    end

    # JD -> (year, day-of-year)
    def jd_to_ordinal(jd, sg)
      y, = jd_to_civil(jd, sg)
      jd_jan1 = civil_to_jd(y, 1, 1, sg)
      [y, jd - jd_jan1 + 1]
    end

    # Commercial (ISO week: cwyear, cweek, cwday) -> JD
    def commercial_to_jd(y, w, d, sg = -Float::INFINITY)
      # Jan 4 is always in week 1 (ISO 8601)
      jd_jan4 = civil_to_jd(y, 1, 4, sg)
      # Monday of week 1
      wday_jan4 = (jd_jan4 + 1) % 7  # 0=Sun
      iso_wday_jan4 = wday_jan4 == 0 ? 7 : wday_jan4
      mon_wk1 = jd_jan4 - (iso_wday_jan4 - 1)
      mon_wk1 + (w - 1) * 7 + (d - 1)
    end

    # JD -> (cwyear, cweek, cwday)
    def jd_to_commercial(jd, sg = -Float::INFINITY)
      wday = (jd + 1) % 7          # 0=Sun
      cwday = wday == 0 ? 7 : wday # 1=Mon..7=Sun
      # Thursday of the same ISO week
      thursday = jd + (4 - cwday)
      y, = thursday >= sg ? jd_to_gregorian(thursday) : jd_to_julian(thursday)
      jd_jan4 = civil_to_jd(y, 1, 4, sg)
      wday_jan4 = (jd_jan4 + 1) % 7
      iso_wday_jan4 = wday_jan4 == 0 ? 7 : wday_jan4
      mon_wk1 = jd_jan4 - (iso_wday_jan4 - 1)
      cweek = (jd - mon_wk1) / 7 + 1
      [y, cweek, cwday]
    end

    # ---------------------------------------------------------------------------
    # Validation helpers
    # ---------------------------------------------------------------------------

    def internal_valid_jd?(jd, _sg)
      jd.is_a?(Numeric) ? jd.to_i : nil
    end

    def internal_valid_civil?(y, m, d, sg)
      return nil unless y.is_a?(Numeric) && m.is_a?(Numeric) && d.is_a?(Numeric)
      y = y.to_i
      m = m.to_i
      d = d.to_i
      # Handle negative month
      m += 13 if m < 0
      return nil if m < 1 || m > 12

      if sg == Float::INFINITY
        # Proleptic Julian calendar.
        dim = days_in_month_julian(y, m)
        d += dim + 1 if d < 0
        return nil if d < 1 || d > dim
        return julian_to_jd(y, m, d)
      end

      # Gregorian reckoning: valid for GREGORIAN and for the post-reform side of
      # a finite cutover. Days skipped by the reform have a Gregorian JD before
      # the cutover (gjd < sg) and are rejected here, then handled below.
      gdim = days_in_month_gregorian(y, m)
      gd = d < 0 ? d + gdim + 1 : d
      if gd >= 1 && gd <= gdim
        gjd = gregorian_to_jd(y, m, gd)
        return gjd if gjd >= sg
      end
      return nil if sg == -Float::INFINITY

      # Pre-reform (Julian) side. This also accepts Julian-only leap days such as
      # 1500-02-29 under ITALY. A day skipped by the reform has jjd >= sg.
      jdim = days_in_month_julian(y, m)
      jd2 = d < 0 ? d + jdim + 1 : d
      return nil if jd2 < 1 || jd2 > jdim
      jjd = julian_to_jd(y, m, jd2)
      jjd < sg ? jjd : nil
    end

    def internal_valid_ordinal?(y, yday, sg)
      return nil unless y.is_a?(Numeric) && yday.is_a?(Numeric)
      y = y.to_i
      yday = yday.to_i
      # The difference of the two Jan 1 Julian Day numbers is the true length of
      # the year under this cutover, including the shortened reform year. Ordinal
      # days are continuous from Jan 1, so a bounds check is sufficient.
      jd_jan1 = civil_to_jd(y, 1, 1, sg)
      diy = civil_to_jd(y + 1, 1, 1, sg) - jd_jan1
      yday += diy + 1 if yday < 0
      return nil if yday < 1 || yday > diy
      jd_jan1 + yday - 1
    end

    def internal_valid_commercial?(y, w, d, sg)
      return nil unless y.is_a?(Numeric) && w.is_a?(Numeric) && d.is_a?(Numeric)
      y = y.to_i
      w = w.to_i
      d = d.to_i
      # ISO cwday: 1=Mon..7=Sun
      d += 8 if d < 0
      return nil if d < 1 || d > 7
      # Weeks in year: Dec 28 is always in the last ISO week
      jd_dec28 = civil_to_jd(y, 12, 28, sg)
      _, max_week, = jd_to_commercial(jd_dec28, sg)
      w += max_week + 1 if w < 0
      return nil if w < 1 || w > max_week
      commercial_to_jd(y, w, d, sg)
    end

    # ---------------------------------------------------------------------------
    # Internal object factory
    # ---------------------------------------------------------------------------

    # Build a Date from a Julian Day Number and start. When the civil fields
    # are already known (e.g. the Date.new fast path), pass them so the new
    # instance can cache them instead of recomputing from the Julian Day.
    # df: the day fraction a Date carries, as the C extension's does.
    def new_from_jd(jd, sg, year = nil, month = nil, day = nil, df = nil)
      obj = allocate
      obj.__send__(:initialize, jd, sg, year, month, day, df)
      obj
    end

    # Parse offset string like "+09:00", "-07:30", "Z" to seconds.
    def offset_str_to_sec(str)
      # Use the full date_zone_to_diff port so named zones (JST, PST, ...),
      # hour-only forms (+09, +9) and the out-of-range check are handled; an
      # unrecognized or out-of-range zone yields 0, which the C extension
      # treats as no offset.
      sp_zone_to_diff(str) || 0
    end

  end

  # ---------------------------------------------------------------------------
  # Instance methods - basic attributes
  # ---------------------------------------------------------------------------

  # call-seq:
  #   year -> integer
  #
  # Returns the year:
  #
  #   Date.new(2001, 2, 3).year    # => 2001
  #   (Date.new(1, 1, 1) - 1).year # => 0
  #
  def year
    internal_civil unless @year
    @year
  end

  # call-seq:
  #   mon -> integer
  #
  # Returns the month in range (1..12):
  #
  #   Date.new(2001, 2, 3).mon # => 2
  #
  def month
    internal_civil unless @year
    @month
  end
  alias mon month

  # call-seq:
  #   mday -> integer
  #
  # Returns the day of the month in range (1..31):
  #
  #   Date.new(2001, 2, 3).mday # => 3
  #
  def day
    internal_civil unless @year
    @day
  end
  alias mday day

  # call-seq:
  #    d.jd  ->  integer
  #
  # Returns the Julian day number.  This is a whole number, which is
  # adjusted by the offset as the local time.
  #
  #    DateTime.new(2001,2,3,4,5,6,'+7').jd	#=> 2451944
  #    DateTime.new(2001,2,3,4,5,6,'-7').jd	#=> 2451944
  def jd
    @jd
  end

  # call-seq:
  #   start -> float
  #
  # Returns the Julian start date for calendar reform;
  # if not an infinity, the returned value is suitable
  # for passing to Date#jd:
  #
  #   d = Date.new(2001, 2, 3, Date::ITALY)
  #   s = d.start     # => 2299161.0
  #   Date.jd(s).to_s # => "1582-10-15"
  #
  #   d = Date.new(2001, 2, 3, Date::ENGLAND)
  #   s = d.start     # => 2361222.0
  #   Date.jd(s).to_s # => "1752-09-14"
  #
  #   Date.new(2001, 2, 3, Date::GREGORIAN).start # => -Infinity
  #   Date.new(2001, 2, 3, Date::JULIAN).start    # => Infinity
  #
  # See argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
  #
  def start
    @start
  end

  # call-seq:
  #    d.ajd  ->  rational
  #
  # Returns the astronomical Julian day number.  This is a fractional
  # number, which is not adjusted by the offset.
  #
  #    DateTime.new(2001,2,3,4,5,6,'+7').ajd	#=> (11769328217/4800)
  #    DateTime.new(2001,2,2,14,5,6,'-7').ajd	#=> (11769328217/4800)
  def ajd
    r = Rational(@jd * 2 - 1, 2)
    r + day_fraction
  end

  # call-seq:
  #    d.amjd  ->  rational
  #
  # Returns the astronomical modified Julian day number.  This is
  # a fractional number, which is not adjusted by the offset.
  #
  #    DateTime.new(2001,2,3,4,5,6,'+7').amjd	#=> (249325817/4800)
  #    DateTime.new(2001,2,2,14,5,6,'-7').amjd	#=> (249325817/4800)
  def amjd
    ajd - Rational(4800001, 2)
  end

  # call-seq:
  #    d.mjd  ->  integer
  #
  # Returns the modified Julian day number.  This is a whole number,
  # which is adjusted by the offset as the local time.
  #
  #    DateTime.new(2001,2,3,4,5,6,'+7').mjd	#=> 51943
  #    DateTime.new(2001,2,3,4,5,6,'-7').mjd	#=> 51943
  def mjd
    @jd - 2400001
  end

  # call-seq:
  #   ld -> integer
  #
  # Returns the
  # {Lilian day number}[https://en.wikipedia.org/wiki/Lilian_date],
  # which is the number of days since the beginning of the Gregorian
  # calendar, October 15, 1582.
  #
  #   Date.new(2001, 2, 3).ld # => 152784
  #
  def ld
    @jd - 2299160
  end

  # call-seq:
  #   yday -> integer
  #
  # Returns the day of the year, in range (1..366):
  #
  #   Date.new(2001, 2, 3).yday # => 34
  #
  def yday
    internal_civil unless @year
    jd_jan1 = self.class.__send__(:civil_to_jd, @year, 1, 1, @start)
    @jd - jd_jan1 + 1
  end

  # call-seq:
  #   wday -> integer
  #
  # Returns the day of week in range (0..6); Sunday is 0:
  #
  #   Date.new(2001, 2, 3).wday # => 6
  #
  def wday
    (@jd + 1) % 7
  end

  # call-seq:
  #   cwday -> integer
  #
  # Returns the commercial-date weekday index for +self+
  # (see Date.commercial);
  # 1 is Monday:
  #
  #   Date.new(2001, 2, 3).cwday # => 6
  #
  def cwday
    w = wday
    w == 0 ? 7 : w
  end

  # call-seq:
  #   cweek -> integer
  #
  # Returns commercial-date week index for +self+
  # (see Date.commercial):
  #
  #   Date.new(2001, 2, 3).cweek # => 5
  #
  def cweek
    compute_commercial[1]
  end

  # call-seq:
  #   cwyear -> integer
  #
  # Returns commercial-date year for +self+
  # (see Date.commercial):
  #
  #   Date.new(2001, 2, 3).cwyear # => 2001
  #   Date.new(2000, 1, 1).cwyear # => 1999
  #
  def cwyear
    compute_commercial[0]
  end

  # call-seq:
  #   day_fraction -> rational
  #
  # Returns the fractional part of the day in range (Rational(0, 1)...Rational(1, 1)):
  #
  #   DateTime.new(2001,2,3,12).day_fraction # => (1/2)
  #
  def day_fraction
    # C returns Integer 0 (INT2FIX(0)) for a simple Date without a fraction,
    # and a Rational only when a day fraction is present.
    @day_fraction || 0
  end

  # call-seq:
  #   leap? -> true or false
  #
  # Returns +true+ if the year is a leap year, +false+ otherwise:
  #
  #   Date.new(2000).leap? # => true
  #   Date.new(2001).leap? # => false
  #
  def leap?
    internal_civil unless @year
    if @jd < @start  # julian?
      @year % 4 == 0
    else
      (@year % 4 == 0 && @year % 100 != 0) || @year % 400 == 0
    end
  end

  # call-seq:
  #   gregorian? -> true or false
  #
  # Returns +true+ if the date is on or after
  # the date of calendar reform, +false+ otherwise:
  #
  #   Date.new(1582, 10, 15).gregorian?       # => true
  #   (Date.new(1582, 10, 15) - 1).gregorian? # => false
  #
  def gregorian?
    jd >= @start
  end

  # call-seq:
  #   d.julian? -> true or false
  #
  # Returns +true+ if the date is before the date of calendar reform,
  # +false+ otherwise:
  #
  #   (Date.new(1582, 10, 15) - 1).julian? # => true
  #   Date.new(1582, 10, 15).julian?       # => false
  #
  def julian?
    !gregorian?
  end

  # call-seq:
  #   new_start(start = Date::ITALY]) -> new_date
  #
  # Returns a copy of +self+ with the given +start+ value:
  #
  #   d0 = Date.new(2000, 2, 3)
  #   d0.julian? # => false
  #   d1 = d0.new_start(Date::JULIAN)
  #   d1.julian? # => true
  #
  # See argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
  #
  def new_start(start = ITALY)
    obj = self.class.allocate
    # Store the cutover as a Float so #start/#hash/marshal stay consistent
    # (see #initialize).
    obj.set_jd_start(@jd, Float(start.to_f))
    obj
  end

  # call-seq:
  #   gregorian -> new_date
  #
  # Equivalent to Date#new_start with argument Date::GREGORIAN.
  def gregorian
    new_start(GREGORIAN)
  end

  # call-seq:
  #   italy -> new_date
  #
  # Equivalent to Date#new_start with argument Date::ITALY.
  #
  def italy
    new_start(ITALY)
  end

  # call-seq:
  #   england -> new_date
  #
  # Equivalent to Date#new_start with argument Date::ENGLAND.
  def england
    new_start(ENGLAND)
  end

  # call-seq:
  #   julian -> new_date
  #
  # Equivalent to Date#new_start with argument Date::JULIAN.
  def julian
    new_start(JULIAN)
  end

  # call-seq:
  #   sunday? -> true or false
  #
  # Returns +true+ if +self+ is a Sunday, +false+ otherwise.
  def sunday?
    wday == 0
  end
  # call-seq:
  #   monday? -> true or false
  #
  # Returns +true+ if +self+ is a Monday, +false+ otherwise.
  def monday?
    wday == 1
  end
  # call-seq:
  #   tuesday? -> true or false
  #
  # Returns +true+ if +self+ is a Tuesday, +false+ otherwise.
  def tuesday?
    wday == 2
  end
  # call-seq:
  #   wednesday? -> true or false
  #
  # Returns +true+ if +self+ is a Wednesday, +false+ otherwise.
  def wednesday?
    wday == 3
  end
  # call-seq:
  #   thursday? -> true or false
  #
  # Returns +true+ if +self+ is a Thursday, +false+ otherwise.
  def thursday?
    wday == 4
  end
  # call-seq:
  #   friday? -> true or false
  #
  # Returns +true+ if +self+ is a Friday, +false+ otherwise.
  def friday?
    wday == 5
  end
  # call-seq:
  #   saturday? -> true or false
  #
  # Returns +true+ if +self+ is a Saturday, +false+ otherwise.
  def saturday?
    wday == 6
  end

  # ---------------------------------------------------------------------------
  # Comparison
  # ---------------------------------------------------------------------------

  # call-seq:
  #   self <=> other  -> -1, 0, 1 or nil
  #
  # Compares +self+ and +other+, returning:
  #
  # - <tt>-1</tt> if +other+ is larger.
  # - <tt>0</tt> if the two are equal.
  # - <tt>1</tt> if +other+ is smaller.
  # - +nil+ if the two are incomparable.
  #
  # Argument +other+ may be:
  #
  # - Another \Date object:
  #
  #     d = Date.new(2022, 7, 27) # => #<Date: 2022-07-27 ((2459788j,0s,0n),+0s,2299161j)>
  #     prev_date = d.prev_day    # => #<Date: 2022-07-26 ((2459787j,0s,0n),+0s,2299161j)>
  #     next_date = d.next_day    # => #<Date: 2022-07-28 ((2459789j,0s,0n),+0s,2299161j)>
  #     d <=> next_date           # => -1
  #     d <=> d                   # => 0
  #     d <=> prev_date           # => 1
  #
  # - A DateTime object:
  #
  #     d <=> DateTime.new(2022, 7, 26) # => 1
  #     d <=> DateTime.new(2022, 7, 27) # => 0
  #     d <=> DateTime.new(2022, 7, 28) # => -1
  #
  # - A numeric (compares <tt>self.ajd</tt> to +other+):
  #
  #     d <=> 2459788 # => -1
  #     d <=> 2459787 # => 1
  #     d <=> 2459786 # => 1
  #     d <=> d.ajd   # => 0
  #
  # - Any other object:
  #
  #     d <=> Object.new # => nil
  #
  def <=>(other)
    case other
    when Date
      d = @jd <=> other.jd
      d != 0 ? d : day_fraction <=> other.day_fraction
    when Numeric
      ajd <=> other
    else
      nil
    end
  end

  # #<, #>, #==, #<=, #>= are all provided by Comparable via #<=>; only #<=>
  # and #=== are defined explicitly.

  def eql?(other)
    other.is_a?(Date) && @jd == other.jd && day_fraction == other.day_fraction
  end

  def hash
    [@jd, @start].hash
  end

  # call-seq:
  #   self === other -> true, false, or nil.
  #
  # Returns +true+ if +self+ and +other+ represent the same date,
  # +false+ if not, +nil+ if the two are not comparable.
  #
  # Argument +other+ may be:
  #
  # - Another \Date object:
  #
  #     d = Date.new(2022, 7, 27) # => #<Date: 2022-07-27 ((2459788j,0s,0n),+0s,2299161j)>
  #     prev_date = d.prev_day    # => #<Date: 2022-07-26 ((2459787j,0s,0n),+0s,2299161j)>
  #     next_date = d.next_day    # => #<Date: 2022-07-28 ((2459789j,0s,0n),+0s,2299161j)>
  #     d === prev_date           # => false
  #     d === d                   # => true
  #     d === next_date           # => false
  #
  # - A DateTime object:
  #
  #     d === DateTime.new(2022, 7, 26) # => false
  #     d === DateTime.new(2022, 7, 27) # => true
  #     d === DateTime.new(2022, 7, 28) # => false
  #
  # - A numeric (compares <tt>self.jd</tt> to +other+):
  #
  #     d === 2459788 # => true
  #     d === 2459787 # => false
  #     d === 2459786 # => false
  #     d === d.jd    # => true
  #
  # - An object not comparable:
  #
  #     d === Object.new # => nil
  #
  def ===(other)
    case other
    when Numeric
      jd == other.to_i
    when Date
      jd == other.jd
    else
      nil
    end
  end

  # ---------------------------------------------------------------------------
  # Arithmetic
  # ---------------------------------------------------------------------------

  # call-seq:
  #    d + other  ->  date
  #
  # Returns a date object pointing +other+ days after self.  The other
  # should be a numeric value.  If the other is a fractional number,
  # assumes its precision is at most nanosecond.
  #
  #    Date.new(2001,2,3) + 1	#=> #<Date: 2001-02-04 ...>
  #    DateTime.new(2001,2,3) + Rational(1,2)
  #				#=> #<DateTime: 2001-02-03T12:00:00+00:00 ...>
  #    DateTime.new(2001,2,3) + Rational(-1,2)
  #				#=> #<DateTime: 2001-02-02T12:00:00+00:00 ...>
  #    DateTime.jd(0,12) + DateTime.new(2001,2,3).ajd
  #				#=> #<DateTime: 2001-02-03T00:00:00+00:00 ...>
  def +(other)
    case other
    when Integer
      _plus_days(other)
    when Numeric
      r = other.to_r
      raise TypeError, "#{other.class} can't be coerced into Integer" unless r.is_a?(Rational)
      _plus_fraction(r)
    else
      raise TypeError, "expected numeric"
    end
  end

  # The construction behind + and -: a Date from its day number and day
  # fraction, a DateTime from its time of day as well (DateTime defines
  # these hooks for itself). CRuby has one + and one - for both classes, so
  # an alias a program takes of either in Date (activesupport's
  # plus_without_duration) names the method a DateTime runs too; these hooks
  # keep it so, where a DateTime#+ of its own would be another method.
  private def _plus_days(n)
    self.class.__send__(:new_from_jd, @jd + n, @start, nil, nil, nil, @day_fraction)
  end

  private def _plus_fraction(r)
    total = r + day_fraction
    days = total.floor
    frac = total - days
    self.class.__send__(:new_from_jd, @jd + days, @start, nil, nil, nil, frac == 0 ? nil : frac)
  end

  # call-seq:
  #    d - other  ->  date or rational
  #
  # If the other is a date object, returns a Rational
  # whose value is the difference between the two dates in days.
  # If the other is a numeric value, returns a date object
  # pointing +other+ days before self.
  # If the other is a fractional number,
  # assumes its precision is at most nanosecond.
  #
  #     Date.new(2001,2,3) - 1	#=> #<Date: 2001-02-02 ...>
  #     DateTime.new(2001,2,3) - Rational(1,2)
  #				#=> #<DateTime: 2001-02-02T12:00:00+00:00 ...>
  #     Date.new(2001,2,3) - Date.new(2001)
  #				#=> (33/1)
  #     DateTime.new(2001,2,3) - DateTime.new(2001,2,2,12)
  #				#=> (1/2)
  def -(other)
    case other
    when Date
      # one answer for Date and DateTime alike, as the C extension's one
      # method gives it: a DateTime's offset counts
      ajd - other.ajd
    when Integer
      _plus_days(-other)
    when Numeric
      r = other.to_r
      raise TypeError, "#{other.class} can't be coerced into Integer" unless r.is_a?(Rational)
      _plus_fraction(-r)
    else
      raise TypeError, "expected numeric"
    end
  end

  # call-seq:
  #   d >> n -> new_date
  #
  # Returns a new \Date object representing the date
  # +n+ months later; +n+ should be a numeric:
  #
  #   (Date.new(2001, 2, 3) >> 1).to_s  # => "2001-03-03"
  #   (Date.new(2001, 2, 3) >> -2).to_s # => "2000-12-03"
  #
  # When the same day does not exist for the new month,
  # the last day of that month is used instead:
  #
  #   (Date.new(2001, 1, 31) >> 1).to_s  # => "2001-02-28"
  #   (Date.new(2001, 1, 31) >> -4).to_s # => "2000-09-30"
  #
  # This results in the following, possibly unexpected, behaviors:
  #
  #   d0 = Date.new(2001, 1, 31)
  #   d1 = d0 >> 1 # => #<Date: 2001-02-28>
  #   d2 = d1 >> 1 # => #<Date: 2001-03-28>
  #
  #   d0 = Date.new(2001, 1, 31)
  #   d1 = d0 >> 1  # => #<Date: 2001-02-28>
  #   d2 = d1 >> -1 # => #<Date: 2001-01-28>
  #
  def >>(n)
    self.class.__send__(:new_from_jd, month_shifted_jd(n), @start)
  end

  # Julian Day number of the date +n+ months from self, clamping the day to the
  # last day of the target month (using the calendar that applies under the
  # cutover). Shared with DateTime#>> so the time of day can be preserved.
  private def month_shifted_jd(n)
    internal_civil unless @year
    m2 = @month + n.to_i
    y2 = @year + (m2 - 1).div(12)
    m2 = (m2 - 1) % 12 + 1
    klass = self.class
    sg = @start
    julian = if sg == Float::INFINITY
               true
             elsif sg == -Float::INFINITY
               false
             else
               klass.__send__(:gregorian_to_jd, y2, m2, 1) < sg
             end
    dim = julian ? klass.__send__(:days_in_month_julian, y2, m2)
                 : klass.__send__(:days_in_month_gregorian, y2, m2)
    d2 = @day < dim ? @day : dim
    klass.__send__(:civil_to_jd, y2, m2, d2, sg)
  end

  # call-seq:
  #    d << n  ->  date
  #
  # Returns a new \Date object representing the date
  # +n+ months earlier; +n+ should be a numeric:
  #
  #   (Date.new(2001, 2, 3) << 1).to_s  # => "2001-01-03"
  #   (Date.new(2001, 2, 3) << -2).to_s # => "2001-04-03"
  #
  # When the same day does not exist for the new month,
  # the last day of that month is used instead:
  #
  #   (Date.new(2001, 3, 31) << 1).to_s  # => "2001-02-28"
  #   (Date.new(2001, 3, 31) << -6).to_s # => "2001-09-30"
  #
  # This results in the following, possibly unexpected, behaviors:
  #
  #   d0 = Date.new(2001, 3, 31)
  #   d0 << 2      # => #<Date: 2001-01-31>
  #   d0 << 1 << 1 # => #<Date: 2001-01-28>
  #
  #   d0 = Date.new(2001, 3, 31)
  #   d1 = d0 << 1  # => #<Date: 2001-02-28>
  #   d2 = d1 << -1 # => #<Date: 2001-03-28>
  #
  def <<(n)
    self >> -n
  end

  # call-seq:
  #   next_day(n = 1) -> new_date
  #
  # Equivalent to Date#+ with argument +n+.
  def next_day(n = 1)
    self + n
  end

  # call-seq:
  #   prev_day(n = 1) -> new_date
  #
  # Equivalent to Date#- with argument +n+.
  def prev_day(n = 1)
    self - n
  end

  # call-seq:
  #   d.next -> new_date
  #
  # Returns a new \Date object representing the following day:
  #
  #   d = Date.new(2001, 2, 3)
  #   d.to_s      # => "2001-02-03"
  #   d.next.to_s # => "2001-02-04"
  #
  def next
    self + 1
  end
  alias_method :succ, :next

  # call-seq:
  #   next_year(n = 1) -> new_date
  #
  # Equivalent to #>> with argument <tt>n * 12</tt>.
  def next_year(n = 1)
    self >> n * 12
  end

  # call-seq:
  #   prev_year(n = 1) -> new_date
  #
  # Equivalent to #<< with argument <tt>n * 12</tt>.
  def prev_year(n = 1)
    self << n * 12
  end

  # call-seq:
  #   next_month(n = 1) -> new_date
  #
  # Equivalent to #>> with argument +n+.
  def next_month(n = 1)
    self >> n
  end

  # call-seq:
  #   prev_month(n = 1) -> new_date
  #
  # Equivalent to #<< with argument +n+.
  def prev_month(n = 1)
    self << n
  end

  # call-seq:
  #   step(limit, step = 1){|date| ... } -> self
  #
  # Calls the block with specified dates;
  # returns +self+.
  #
  # - The first +date+ is +self+.
  # - Each successive +date+ is <tt>date + step</tt>,
  #   where +step+ is the numeric step size in days.
  # - The last date is the last one that is before or equal to +limit+,
  #   which should be a \Date object.
  #
  # Example:
  #
  #   limit = Date.new(2001, 12, 31)
  #   Date.new(2001).step(limit){|date| p date.to_s if date.mday == 31 }
  #
  # Output:
  #
  #   "2001-01-31"
  #   "2001-03-31"
  #   "2001-05-31"
  #   "2001-07-31"
  #   "2001-08-31"
  #   "2001-10-31"
  #   "2001-12-31"
  #
  # Returns an Enumerator if no block is given.
  def step(limit, step = 1)
    return to_enum(:step, limit, step) unless block_given?
    if Integer === step && instance_of?(Date) && limit.instance_of?(Date)
      raise ArgumentError, "step can't be 0" if step == 0
      limit_jd = limit.jd
      sg = @start
      if step > 0
        jd = @jd
        while jd <= limit_jd
          obj = Date.allocate
          obj.set_jd_start(jd, sg)
          yield obj
          jd += step
        end
      else
        jd = @jd
        while jd >= limit_jd
          obj = Date.allocate
          obj.set_jd_start(jd, sg)
          yield obj
          jd += step
        end
      end
      return self
    end
    d = self
    cmp = step <=> 0
    raise ArgumentError, "comparison of #{step.class} with 0 failed" if cmp.nil?
    if cmp > 0
      while d <= limit
        yield d
        d = d + step
      end
    elsif cmp < 0
      while d >= limit
        yield d
        d = d + step
      end
    else
      raise ArgumentError, "step can't be 0"
    end
    self
  end

  # call-seq:
  #   upto(max){|date| ... } -> self
  #
  # Equivalent to #step with arguments +max+ and +1+.
  def upto(max, &block)
    return to_enum(:upto, max) unless block_given?
    if instance_of?(Date) && max.instance_of?(Date)
      jd = @jd
      max_jd = max.jd
      sg = @start
      while jd <= max_jd
        obj = Date.allocate
        obj.set_jd_start(jd, sg)
        yield obj
        jd += 1
      end
      return self
    end
    step(max, 1, &block)
  end

  # call-seq:
  #   downto(min){|date| ... } -> self
  #
  # Equivalent to #step with arguments +min+ and <tt>-1</tt>.
  def downto(min, &block)
    return to_enum(:downto, min) unless block_given?
    if instance_of?(Date) && min.instance_of?(Date)
      jd = @jd
      min_jd = min.jd
      sg = @start
      while jd >= min_jd
        obj = Date.allocate
        obj.set_jd_start(jd, sg)
        yield obj
        jd -= 1
      end
      return self
    end
    step(min, -1, &block)
  end

  # ---------------------------------------------------------------------------
  # Calendar conversion
  # ---------------------------------------------------------------------------

  # call-seq:
  #   to_date -> self
  #
  # Returns +self+.
  def to_date
    self
  end

  # call-seq:
  #    d.to_datetime  -> datetime
  #
  # Returns a DateTime whose value is the same as +self+:
  #
  #   Date.new(2001, 2, 3).to_datetime # => #<DateTime: 2001-02-03T00:00:00+00:00>
  #
  def to_datetime
    DateTime.new(year, month, day, 0, 0, 0, 0, @start)
  end

  # call-seq:
  #   to_time -> time
  #
  # Returns a new Time object with the same value as +self+;
  # if +self+ is a Julian date, derives its Gregorian date
  # for conversion to the \Time object:
  #
  #   Date.new(2001, 2, 3).to_time               # => 2001-02-03 00:00:00 -0600
  #   Date.new(2001, 2, 3, Date::JULIAN).to_time # => 2001-02-16 00:00:00 -0600
  #
  def to_time
    year, month, day = self.class.__send__(:jd_to_gregorian, @jd)
    Time.local(year, month, day)
  end

  # ---------------------------------------------------------------------------
  # Serialization
  # ---------------------------------------------------------------------------

  # :nodoc:
  # @rbs other: Date
  def initialize_copy(other)
    # Assign every ivar in the canonical order so copies share the same shape
    # as freshly constructed instances (see #initialize). This stays an explicit
    # copy rather than delegating to #initialize: DateTime overrides #initialize
    # with an incompatible (year, month, day, ...) signature, so a shared
    # initialize_copy must not dispatch through it. The fields are read through
    # the readers below, not instance_variable_get, so the compiler knows what
    # each one holds.
    @jd            = Integer(other.copied_jd)
    @start         = Float(other.copied_start)
    @day_fraction  = other.copied_day_fraction
    @year          = other.copied_year
    @month         = other.copied_month
    @day           = other.copied_day
  end

  protected

  # The fields initialize_copy reads from the Date it copies.
  def copied_jd = @jd
  def copied_start = @start
  def copied_day_fraction = @day_fraction
  def copied_year = @year
  def copied_month = @month
  def copied_day = @day

  # Sets the Julian day and the cutover of a Date allocate made (and
  # nothing else), without instance_variable_set.
  # @rbs jd: Integer
  # @rbs sg: Float
  def set_jd_start(jd, sg)
    @jd = jd
    @start = sg
    self
  end

  public

  # :nodoc:
  def marshal_dump
    # 6-element format: [nth, jd, df, sf, of, sg]
    # df = seconds into day (Integer), sf = sub-second fraction (Rational)
    if @day_fraction
      total_sec = @day_fraction * 86400
      df_int = total_sec.floor
      sf = total_sec - df_int
      [0, @jd, df_int, sf, 0, @start]
    else
      [0, @jd, 0, 0, 0, @start]
    end
  end

  # :nodoc:
  def marshal_load(array)
    case array.length
    when 2
      # Format 1.4/1.6: [jd_like, sg_or_bool]
      jd_like, sg_or_bool = array
      sg = sg_or_bool == true ? GREGORIAN : (sg_or_bool == false ? JULIAN : sg_or_bool.to_f)
      initialize(jd_like.to_i, sg)
    when 3
      # Format 1.8: [ajd, of, sg]
      ajd, _of, sg = array
      sg = normalize_start(sg)
      raw_jd = ajd + 0.5r
      jd = raw_jd.floor
      df = raw_jd - jd
      initialize(jd, sg, nil, nil, nil, df == 0 ? nil : df)
    when 6
      # Current format: [nth, jd, df, sf, of, sg]
      _nth, jd, df, sf, _of, sg = array
      sg = normalize_start(sg)
      if df != 0 || sf != 0
        initialize(jd, sg, nil, nil, nil, (Rational(df) + sf) / 86400)
      else
        initialize(jd, sg)
      end
    else
      raise TypeError, "invalid marshal data"
    end
    self
  end

  #  call-seq:
  #    deconstruct_keys(array_of_names_or_nil) -> hash
  #
  #  Returns a hash of the name/value pairs, to use in pattern matching.
  #  Possible keys are: <tt>:year</tt>, <tt>:month</tt>, <tt>:day</tt>,
  #  <tt>:wday</tt>, <tt>:yday</tt>.
  #
  #  Possible usages:
  #
  #    d = Date.new(2022, 10, 5)
  #
  #    if d in wday: 3, day: ..7  # uses deconstruct_keys underneath
  #      puts "first Wednesday of the month"
  #    end
  #    #=> prints "first Wednesday of the month"
  #
  #    case d
  #    in year: ...2022
  #      puts "too old"
  #    in month: ..9
  #      puts "quarter 1-3"
  #    in wday: 1..5, month:
  #      puts "working day in month #{month}"
  #    end
  #    #=> prints "working day in month 10"
  #
  #  Note that deconstruction by pattern can also be combined with class check:
  #
  #    if d in Date(wday: 3, day: ..7)
  #      puts "first Wednesday of the month"
  #    end
  #
  def deconstruct_keys(keys)
    if keys
      if keys.size == 1
        case keys[0]
        when :year  then { year: }
        when :month then { month: }
        when :day   then { day: }
        when :wday  then { wday: }
        when :yday  then { yday: }
        else {}
        end
      else
        h = {}
        keys.each do |k|
          case k
          when :year  then h[:year] = year
          when :month then h[:month] = month
          when :day   then h[:day] = day
          when :wday  then h[:wday] = wday
          when :yday  then h[:yday] = yday
          end
          nil  # (each drops the block's value; nil keeps the fields' values out of it)
        end
        h
      end
    else
      { year:, month:, day:, wday:, yday: }
    end
  end

  # ---------------------------------------------------------------------------
  # String formatting (delegated to strftime.rb)
  # ---------------------------------------------------------------------------

  # call-seq:
  #   asctime -> string
  #
  # Equivalent to #strftime with argument <tt>'%a %b %e %T %Y'</tt>
  # (or its {shorthand form}[rdoc-ref:language/strftime_formatting.rdoc@Shorthand+Conversion+Specifiers]
  # <tt>'%c'</tt>):
  #
  #   Date.new(2001, 2, 3).asctime # => "Sat Feb  3 00:00:00 2001"
  #
  # See {asctime}[https://linux.die.net/man/3/asctime].
  #
  def asctime
    internal_civil unless @year
    d = @day
    d_s = d < 10 ? " #{d}" : d.to_s
    y = @year
    y_s = y >= 1000 ? y.to_s : (y >= 0 ? format('%04d', y) : format('-%04d', -y))
    w = (@jd + 1) % 7
    if instance_of?(Date)
      "#{ABBR_DAYNAMES[w]} #{ABBR_MONTHNAMES[@month]} #{d_s} 00:00:00 #{y_s}"
    else
      "#{ABBR_DAYNAMES[w]} #{ABBR_MONTHNAMES[@month]} #{d_s} #{PAD2[internal_hour]}:#{PAD2[internal_min]}:#{PAD2[internal_sec]} #{y_s}"
    end
  end
  alias_method :ctime, :asctime

  # call-seq:
  #   rfc3339 -> string
  #
  # Equivalent to #strftime with argument <tt>'%FT%T%:z'</tt>;
  # see {Formats for Dates and Times}[rdoc-ref:language/strftime_formatting.rdoc]:
  #
  #   Date.new(2001, 2, 3).rfc3339 # => "2001-02-03T00:00:00+00:00"
  #
  def rfc3339
    to_s << 'T00:00:00+00:00'
  end

  # call-seq:
  #   rfc2822 -> string
  #
  # Equivalent to #strftime with argument <tt>'%a, %-d %b %Y %T %z'</tt>;
  # see {Formats for Dates and Times}[rdoc-ref:language/strftime_formatting.rdoc]:
  #
  #   Date.new(2001, 2, 3).rfc2822 # => "Sat, 3 Feb 2001 00:00:00 +0000"
  #
  def rfc2822
    internal_civil unless @year
    w = (@jd + 1) % 7
    y = @year
    y_s = y >= 1000 ? y.to_s : (y >= 0 ? format('%04d', y) : format('-%04d', -y))
    if instance_of?(Date)
      "#{ABBR_DAYNAMES[w]}, #{@day}#{RFC_MON_SPACE[@month]}#{y_s} 00:00:00 +0000"
    else
      "#{ABBR_DAYNAMES[w]}, #{@day}#{RFC_MON_SPACE[@month]}#{y_s} #{PAD2[internal_hour]}:#{PAD2[internal_min]}:#{PAD2[internal_sec]} +0000"
    end
  end
  alias_method :rfc822, :rfc2822

  # call-seq:
  #   httpdate -> string
  #
  # Equivalent to #strftime with argument <tt>'%a, %d %b %Y %T GMT'</tt>;
  # see {Formats for Dates and Times}[rdoc-ref:language/strftime_formatting.rdoc]:
  #
  #   Date.new(2001, 2, 3).httpdate # => "Sat, 03 Feb 2001 00:00:00 GMT"
  #
  def httpdate
    internal_civil unless @year
    w = (@jd + 1) % 7
    y = @year
    y_s = y >= 1000 ? y.to_s : (y >= 0 ? format('%04d', y) : format('-%04d', -y))
    if instance_of?(Date)
      "#{ABBR_DAYNAMES[w]}, #{PAD2[@day]}#{RFC_MON_SPACE[@month]}#{y_s} 00:00:00 GMT"
    else
      "#{ABBR_DAYNAMES[w]}, #{PAD2[@day]}#{RFC_MON_SPACE[@month]}#{y_s} #{PAD2[internal_hour]}:#{PAD2[internal_min]}:#{PAD2[internal_sec]} GMT"
    end
  end

  # call-seq:
  #   jisx0301 -> string
  #
  # Returns a string representation of the date in +self+
  # in JIS X 0301 format.
  #
  #   Date.new(2001, 2, 3).jisx0301 # => "H13.02.03"
  #
  def jisx0301
    internal_civil unless @year
    jd = @jd
    m = @month
    d = @day
    md = "#{PAD2[m]}.#{PAD2[d]}"
    if jd >= 2458605       # Reiwa (2019-05-01)
      "R#{PAD2[@year - 2018]}.#{md}"
    elsif jd >= 2447535    # Heisei (1989-01-08)
      "H#{PAD2[@year - 1988]}.#{md}"
    elsif jd >= 2424875    # Showa (1926-12-25)
      "S#{PAD2[@year - 1925]}.#{md}"
    elsif jd >= 2419614    # Taisho (1912-07-30)
      "T#{PAD2[@year - 1911]}.#{md}"
    elsif jd >= 2405160    # Meiji (1873-01-01)
      "M#{PAD2[@year - 1867]}.#{md}"
    else
      to_s
    end
  end

  # call-seq:
  #   to_s -> string
  #
  # Returns a string representation of the date in +self+
  # in {ISO 8601 extended date format}[rdoc-ref:language/strftime_formatting.rdoc@ISO+8601+Format+Specifications]
  # (<tt>'%Y-%m-%d'</tt>):
  #
  #   Date.new(2001, 2, 3).to_s # => "2001-02-03"
  #
  def to_s
    internal_civil
    suffix = MONTH_DAY_SUFFIX[@month][@day]
    y = @year
    if y >= 1000
      # Fast path: 4-digit year needs no zero-padding (most common case).
      y.to_s << suffix
    elsif y >= 0
      format('%04d', y) << suffix
    else
      format('-%04d', -y) << suffix
    end
  end

  # call-seq:
  #   iso8601    ->  string
  #
  # Equivalent to #strftime with argument <tt>'%Y-%m-%d'</tt>
  # (or its {shorthand form}[rdoc-ref:language/strftime_formatting.rdoc@Shorthand+Conversion+Specifiers]
  # <tt>'%F'</tt>);
  #
  #   Date.new(2001, 2, 3).iso8601 # => "2001-02-03"
  #
  alias_method :iso8601, :to_s
  alias_method :xmlschema, :iso8601

  # call-seq:
  #   inspect -> string
  #
  # Returns a string representation of +self+:
  #
  #   Date.new(2001, 2, 3).inspect
  #   # => "#<Date: 2001-02-03 ((2451944j,0s,0n),+0s,2299161j)>"
  #
  def inspect
    "#<Date: #{to_s} ((#{@jd}j,0s,0n),+0s,#{inspect_sg}j)>"
  end

  # Format the start (cutover) for #inspect: "Inf"/"-Inf" for the infinite
  # sentinels and an integer for whole values.
  private def inspect_sg
    sg = @start
    if sg == Float::INFINITY
      'Inf'
    elsif sg == -Float::INFINITY
      '-Inf'
    elsif sg == sg.to_i
      sg.to_i.to_s
    else
      sg.to_s
    end
  end

  # override
  def freeze
    internal_civil  # compute and cache civil date before freezing
    super
  end

  # ---------------------------------------------------------------------------
  # Private helpers
  # ---------------------------------------------------------------------------

  private

  # Normalize a start value coming from old marshal formats: a Date::Infinity
  # object becomes the equivalent Float (start is stored as a Float/Integer
  # internally).
  def normalize_start(sg)
    sg.is_a?(Infinity) ? sg.to_f : sg
  end

  def initialize(jd, sg, year = nil, month = nil, day = nil, df = nil)
    @jd            = Integer(jd)
    # Store the cutover as a Float. Keeps #start, #hash and marshal output
    # consistent regardless of whether the caller passed an Integer (e.g. ITALY)
    # or a Float sentinel.
    @start         = Float(sg.to_f)
    @day_fraction  = df
    # Cache the civil fields when the caller already knows them (Date.new via
    # the fast path passes them), so #year/#month/#day need not recompute them.
    # When unknown they stay nil and are computed lazily by #internal_civil.
    # Every instance assigns all three here, so the object shape is stable
    # regardless of which path constructed it. (yday/cweek/cwyear are not
    # cached; see #yday/#cweek.)
    @year          = year
    @month         = month
    @day           = day
  end

  def internal_civil
    return if @year
    jd = @jd
    if @start == Float::INFINITY       # always Julian
      b = 0
      c = jd + 32082
    elsif jd >= @start                 # Gregorian (handles -Infinity too)
      a = jd + 32044
      b = (4 * a + 3) / 146097
      c = a - (146097 * b) / 4
    else                            # Julian (before reform date)
      b = 0
      c = jd + 32082
    end
    d = (4 * c + 3) / 1461
    e = c - (1461 * d) / 4
    m = (5 * e + 2) / 153
    @day   = e - (153 * m + 2) / 5 + 1
    @month = m + 3 - 12 * (m / 10)
    @year  = 100 * b + d - 4800 + m / 10
  end

  # Compute [cwyear, cweek] from the Julian Day. Recomputed on each access
  # rather than cached, since commercial fields are rarely read repeatedly.
  def compute_commercial
    y, cw, = self.class.__send__(:jd_to_commercial, @jd, @start)
    [y, cw]
  end

end
