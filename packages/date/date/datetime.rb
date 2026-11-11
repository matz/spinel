# frozen_string_literal: true

class DateTime < Date

  # Sentinel marking an hour/minute/second argument that was not passed, so a
  # trailing fraction can be rejected only when a smaller unit was actually given.
  UNSET = Object.new
  private_constant :UNSET

  # ---------------------------------------------------------------------------
  # Initializer
  # ---------------------------------------------------------------------------

  # call-seq:
  #   DateTime.new(year=-4712, month=1, day=1, hour=0, minute=0, second=0, offset=0, start=Date::ITALY) -> datetime
  def initialize(year = -4712, month = 1, day = 1, hour = UNSET, minute = UNSET, second = UNSET, offset = 0, start = ITALY)
    hour_given   = !UNSET.equal?(hour)
    minute_given = !UNSET.equal?(minute)
    second_given = !UNSET.equal?(second)
    hour   = 0 if UNSET.equal?(hour)
    minute = 0 if UNSET.equal?(minute)
    second = 0 if UNSET.equal?(second)

    # Validate argument types in the C extension's order (offset, then second
    # down to year) so the same field is reported first when several are bad.
    of_sec = self.class.__send__(:_parse_of, offset)
    raise TypeError, "invalid second (not numeric)" unless second.is_a?(Numeric)
    raise TypeError, "invalid minute (not numeric)" unless minute.is_a?(Numeric)
    raise TypeError, "invalid hour (not numeric)" unless hour.is_a?(Numeric)
    raise TypeError, "invalid day (not numeric)" unless day.is_a?(Numeric)
    raise TypeError, "invalid month (not numeric)" unless month.is_a?(Numeric)
    month = Integer(month)
    raise TypeError, "invalid year (not numeric)" unless year.is_a?(Numeric)
    year  = Integer(year)

    # A fractional day/hour/minute is only allowed as the last argument given;
    # if a smaller unit was also supplied, the C extension rejects the fraction.
    # These checks run before the date-validity check, as in the C extension.
    day_r    = day.to_r
    day_frac = day_r - day_r.floor
    hour_r0  = hour.to_r
    hour_f0  = hour_r0 - hour_r0.floor
    minute_r0 = minute.to_r
    minute_f0 = minute_r0 - minute_r0.floor
    raise Date::Error, "invalid fraction" if day_frac  != 0 && (hour_given || minute_given || second_given)
    raise Date::Error, "invalid fraction" if hour_f0   != 0 && (minute_given || second_given)
    raise Date::Error, "invalid fraction" if minute_f0 != 0 && second_given

    day = day_r.floor
    jd = self.class.__send__(:internal_valid_civil?, year, month, day, start)
    raise Date::Error, "invalid date" unless jd

    # Propagate fractions to smaller units
    hour_r    = hour_r0 + day_frac * 24
    hour_i    = hour_r.floor
    hour_frac = hour_r - hour_i

    minute_r    = minute_r0 + hour_frac * 60
    minute_i    = minute_r.floor
    minute_frac = minute_r - minute_i

    second_r  = second.to_r + minute_frac * 60
    sec_i     = second_r.floor
    sec_f     = second_r - sec_i
    jd, hour, minute, sec_i = self.class.__send__(:_normalize_hms, jd, hour_i, minute_i, sec_i)

    _init_datetime(jd, hour, minute, sec_i, sec_f, of_sec, start)
  end

  # ---------------------------------------------------------------------------
  # Instance attributes
  # ---------------------------------------------------------------------------

  # call-seq:
  #   hour -> integer
  #
  # Returns the hour in range (0..23):
  #
  #   DateTime.new(2001, 2, 3, 4, 5, 6).hour # => 4
  attr_reader :hour

  # call-seq:
  #   min -> integer
  #
  # Returns the minute in range (0..59):
  #
  #   DateTime.new(2001, 2, 3, 4, 5, 6).min # => 5
  attr_reader :min
  alias minute min

  # call-seq:
  #   sec -> integer
  #
  # Returns the second in range (0..59):
  #
  #   DateTime.new(2001, 2, 3, 4, 5, 6).sec # => 6
  attr_reader :sec
  alias second sec

  # call-seq:
  #   sec_fraction -> rational
  #
  # Returns the fractional part of the second in range
  # (Rational(0, 1)...Rational(1, 1)):
  #
  #   DateTime.new(2001, 2, 3, 4, 5, 6.5).sec_fraction # => (1/2)
  attr_reader :sec_fraction
  alias second_fraction sec_fraction

  # call-seq:
  #    d.offset  ->  rational
  #
  # Returns the offset.
  #
  #    DateTime.parse('04pm+0730').offset	#=> (5/16)
  def offset
    Rational(@of, 86400)
  end

  # call-seq:
  #    d.zone  ->  string
  #
  # Returns the timezone.
  #
  #    DateTime.parse('04pm+0730').zone		#=> "+07:30"
  def zone
    sign = @of < 0 ? '-' : '+'
    abs  = @of.abs
    h    = abs / 3600
    m    = (abs % 3600) / 60
    format('%s%02d:%02d'.encode(Encoding::US_ASCII), sign, h, m)
  end

  # call-seq:
  #   day_fraction -> rational
  #
  # Returns the fractional part of the day in range (Rational(0, 1)...Rational(1, 1)):
  #
  #   DateTime.new(2001,2,3,12).day_fraction # => (1/2)
  def day_fraction
    Rational(@hour * 3600 + @min * 60 + @sec, 86400) +
      Rational(@sec_fraction.numerator, @sec_fraction.denominator * 86400)
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
    Rational(@jd) + day_fraction - offset - 0.5r
  end

  # ---------------------------------------------------------------------------
  # Arithmetic (override for fractional day support)
  # ---------------------------------------------------------------------------

  # DateTime#+ and DateTime#- are Date's (one method for both classes, as
  # CRuby has it): the construction they come down to is here, from the day
  # number and the time of day.
  private def _plus_days(n)
    self.class.__send__(:_new_dt_from_jd_time, @jd + n, @hour, @min, @sec, @sec_fraction, @of, @start)
  end

  private def _plus_fraction(r)
    extra_sec = r * 86400
    total_r   = Rational(@jd) * 86400 + @hour * 3600 + @min * 60 + @sec + @sec_fraction + extra_sec
    _from_total_sec_r(total_r)
  end

  # call-seq:
  #    dt >> n  ->  date_time
  #
  # Returns a new DateTime object representing the time +n+ months later;
  # the time of day and offset are preserved. (Date#<< delegates to this.)
  def >>(n)
    self.class.__send__(:_new_dt_from_jd_time, month_shifted_jd(n), @hour, @min, @sec, @sec_fraction, @of, @start)
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
  def new_start(start = Date::ITALY)
    self.class.__send__(:_new_dt_from_jd_time, @jd, @hour, @min, @sec, @sec_fraction, @of, start)
  end

  # call-seq:
  #    d.new_offset([offset=0])  ->  date
  #
  # Duplicates self and resets its offset.
  #
  #    d = DateTime.new(2001,2,3,4,5,6,'-02:00')
  #				#=> #<DateTime: 2001-02-03T04:05:06-02:00 ...>
  #    d.new_offset('+09:00')	#=> #<DateTime: 2001-02-03T15:05:06+09:00 ...>
  def new_offset(of = 0)
    of_sec = self.class.__send__(:_parse_of, of)
    # Keep the same absolute instant, re-expressed in the new offset: shift the
    # locally stored wall-clock fields by the change in offset.
    total = @jd * 86400 + @hour * 3600 + @min * 60 + @sec + (of_sec - @of)
    jd2, rem = total.divmod(86400)
    h, rem = rem.divmod(3600)
    m, s = rem.divmod(60)
    self.class.__send__(:_new_dt_from_jd_time, jd2, h, m, s, @sec_fraction, of_sec, @start)
  end

  # ---------------------------------------------------------------------------
  # String formatting
  # ---------------------------------------------------------------------------

  # call-seq:
  #   strftime(format = '%FT%T%:z') -> string
  #
  # Returns a string representation of +self+,
  # formatted according the given +format:
  #
  #   DateTime.now.strftime # => "2022-07-01T11:03:19-05:00"
  #
  # For other formats,
  # see {Formats for Dates and Times}[rdoc-ref:language/strftime_formatting.rdoc]:
  def strftime(format = '%FT%T%:z'.encode(Encoding::US_ASCII))
    super(format)
  end

  # call-seq:
  #    dt.jisx0301([n=0])  ->  string
  #
  # Returns a string in a JIS X 0301 format.
  # The optional argument +n+ is the number of digits for fractional seconds.
  #
  #    DateTime.parse('2001-02-03T04:05:06.123456789+07:00').jisx0301(9)
  #				#=> "H13.02.03T04:05:06.123456789+07:00"
  def jisx0301(n = 0)
    n = _num_to_int(n)
    ERA_TABLE.each do |start_jd, era, base_year|
      if @jd >= start_jd
        era_year = year - base_year
        if n <= 0
          return format('%s%02d.%02d.%02dT%02d:%02d:%02d%s',
                        era, era_year, month, day, hour, min, sec, zone)
        else
          sf   = sec_fraction
          frac = '.' + (sf * (10**n)).to_i.to_s.rjust(n, '0')
          return format('%s%02d.%02d.%02dT%02d:%02d:%02d%s%s',
                        era, era_year, month, day, hour, min, sec, frac, zone)
        end
      end
    end
    iso8601(n)
  end

  # call-seq:
  #    dt.iso8601([n=0])    ->  string
  #    dt.xmlschema([n=0])  ->  string
  #
  # This method is equivalent to strftime('%FT%T%:z').
  # The optional argument +n+ is the number of digits for fractional seconds.
  #
  #    DateTime.parse('2001-02-03T04:05:06.123456789+07:00').iso8601(9)
  #				#=> "2001-02-03T04:05:06.123456789+07:00"
  def iso8601(n = 0)
    n = _num_to_int(n)
    if n <= 0
      strftime('%Y-%m-%dT%H:%M:%S%:z')
    else
      sf   = sec_fraction
      frac = '.' + (sf * (10**n)).to_i.to_s.rjust(n, '0')
      strftime("%Y-%m-%dT%H:%M:%S#{frac}%:z")
    end
  end
  alias_method :xmlschema, :iso8601

  # call-seq:
  #    dt.rfc3339([n=0])  ->  string
  #
  # This method is equivalent to strftime('%FT%T%:z').
  # The optional argument +n+ is the number of digits for fractional seconds.
  #
  #    DateTime.parse('2001-02-03T04:05:06.123456789+07:00').rfc3339(9)
  #				#=> "2001-02-03T04:05:06.123456789+07:00"
  alias_method :rfc3339,   :iso8601

  #  call-seq:
  #    deconstruct_keys(array_of_names_or_nil) -> hash
  #
  #  Returns a hash of the name/value pairs, to use in pattern matching.
  #  Possible keys are: <tt>:year</tt>, <tt>:month</tt>, <tt>:day</tt>,
  #  <tt>:wday</tt>, <tt>:yday</tt>, <tt>:hour</tt>, <tt>:min</tt>,
  #  <tt>:sec</tt>, <tt>:sec_fraction</tt>, <tt>:zone</tt>.
  #
  #  Possible usages:
  #
  #    dt = DateTime.new(2022, 10, 5, 13, 30)
  #
  #    if d in wday: 1..5, hour: 10..18  # uses deconstruct_keys underneath
  #      puts "Working time"
  #    end
  #    #=> prints "Working time"
  #
  #    case dt
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
  #    if d in DateTime(wday: 1..5, hour: 10..18, day: ..7)
  #      puts "Working time, first week of the month"
  #    end
  def deconstruct_keys(keys)
    if keys
      if keys.size == 1
        case keys[0]
        when :year         then { year: }
        when :month        then { month: }
        when :day          then { day: }
        when :wday         then { wday: }
        when :yday         then { yday: }
        when :hour         then { hour: }
        when :min          then { min: }
        when :sec          then { sec: }
        when :sec_fraction then { sec_fraction: }
        when :zone         then { zone: }
        else {}
        end
      else
        h = {}
        keys.each do |k|
          case k
          when :year         then h[:year] = year
          when :month        then h[:month] = month
          when :day          then h[:day] = day
          when :wday         then h[:wday] = wday
          when :yday         then h[:yday] = yday
          when :hour         then h[:hour] = hour
          when :min          then h[:min] = min
          when :sec          then h[:sec] = sec
          when :sec_fraction then h[:sec_fraction] = sec_fraction
          when :zone         then h[:zone] = zone
          end
          nil  # (each drops the block's value; nil keeps the fields' values out of it)
        end
        h
      end
    else
      { year:, month:, day:, wday:, yday:,
        hour:, min:, sec:, sec_fraction:, zone: }
    end
  end

  # call-seq:
  #    dt.to_s  ->  string
  #
  # Returns a string in an ISO 8601 format. (This method doesn't use the
  # expanded representations.)
  #
  #     DateTime.new(2001,2,3,4,5,6,'-7').to_s
  #				#=> "2001-02-03T04:05:06-07:00"
  def to_s
    strftime('%Y-%m-%dT%H:%M:%S%:z'.encode(Encoding::US_ASCII))
  end

  # call-seq:
  #   inspect -> string
  #
  # Returns a string representation of +self+:
  #
  #   DateTime.new(2001,2,3,4,5,6,'+7').inspect
  #   # => "#<DateTime: 2001-02-03T04:05:06+07:00 ((2451943j,75906s,0n),+25200s,2299161j)>"
  #
  # The Julian Day number and seconds are given in UTC; the offset and start
  # (cutover) follow.
  def inspect
    utc_total = @jd * 86400 + @hour * 3600 + @min * 60 + @sec - @of
    utc_jd, utc_sec = utc_total.divmod(86400)
    nano = @sec_fraction * 1_000_000_000
    nano_s = nano.denominator == 1 ? nano.numerator.to_s : "(#{nano.numerator}/#{nano.denominator})"
    of_s = @of < 0 ? "-#{-@of}" : "+#{@of}"
    "#<DateTime: #{to_s} ((#{utc_jd}j,#{utc_sec}s,#{nano_s}n),#{of_s}s,#{inspect_sg}j)>"
  end

  def hash
    if @hour == 0 && @min == 0 && @sec == 0
      [@jd, @start].hash
    else
      [@jd, @hour, @min, @sec, @start].hash
    end
  end

  # ---------------------------------------------------------------------------
  # Serialization override
  # ---------------------------------------------------------------------------

  # :nodoc:
  def marshal_dump
    # 6-element format: [nth, jd, df, sf, of, sg]. Like the C extension, jd and
    # df hold the UTC instant (local time minus the offset), and sf is the
    # sub-second part scaled to nanoseconds, kept as an Integer when whole.
    jd_carry, df = (@hour * 3600 + @min * 60 + @sec - @of).divmod(86400)
    sf = @sec_fraction * 1_000_000_000
    sf = sf.numerator if sf.denominator == 1
    [0, @jd + jd_carry, df, sf, @of, @start]
  end

  # :nodoc:
  def marshal_load(array)
    case array.length
    when 2
      jd_like, sg_or_bool = array
      sg = sg_or_bool == true ? ITALY : (sg_or_bool == false ? JULIAN : sg_or_bool.to_f)
      _init_datetime(jd_like.to_i, 0, 0, 0, 0r, 0, sg)
    when 3
      ajd, of_r, sg = array
      sg = normalize_start(sg)
      of_sec = (of_r * 86400).to_i
      # Reconstruct local JD and time from AJD
      local_r = ajd + 0.5r + of_r
      jd      = local_r.floor
      rem_r   = (local_r - jd) * 86400
      h       = rem_r.to_i / 3600
      rem_r -= h * 3600
      m       = rem_r.to_i / 60
      s_r = rem_r - m * 60
      s_i, s_f = _split_second(s_r)
      _init_datetime(jd, h, m, s_i, s_f, of_sec, sg)
    when 6
      _nth, jd, df, sf, of, sg = array
      sg = normalize_start(sg)
      # jd/df hold the UTC instant; shift by the offset to recover local time.
      jd_carry, local_df = (df + of).divmod(86400)
      jd += jd_carry
      h  = local_df / 3600
      m  = (local_df % 3600) / 60
      s  = local_df % 60
      sf_r = sf.is_a?(Rational) ? (sf / 1_000_000_000) : Rational(sf.to_i, 1_000_000_000)
      _init_datetime(jd, h, m, s, sf_r, of, sg)
    else
      raise TypeError, "invalid marshal data"
    end
    self
  end

  # ---------------------------------------------------------------------------
  # Type conversions
  # ---------------------------------------------------------------------------

  # call-seq:
  #    dt.to_date  ->  date
  #
  # Returns a Date object which denotes self.
  def to_date
    Date.__send__(:new_from_jd, @jd, @start)
  end

  # call-seq:
  #    dt.to_datetime  ->  self
  #
  # Returns self.
  def to_datetime
    self
  end

  # call-seq:
  #    dt.to_time  ->  time
  #
  # Returns a Time object which denotes self.
  def to_time
    y, m, d = self.class.__send__(:jd_to_gregorian, @jd)
    if @of == 0
      Time.utc(y, m, d, @hour, @min, @sec + @sec_fraction)
    else
      Time.new(y, m, d, @hour, @min, @sec + @sec_fraction, @of)
    end
  end

  # ---------------------------------------------------------------------------
  # Class methods
  # ---------------------------------------------------------------------------

  class << self
    def new(*args)
      instance = allocate
      instance.__send__(:initialize, *args)
      instance
    end
    alias_method :civil, :new

    undef_method :today

    # call-seq:
    #    DateTime._strptime(string[, format='%FT%T%z'])  ->  hash
    #
    # Parses the given representation of date and time with the given
    # template, and returns a hash of parsed elements.  _strptime does
    # not support specification of flags and width unlike strftime.
    #
    # See also strptime(3) and #strftime.
    def _strptime(string, format = '%FT%T%z')
      Date._strptime(string, format)
    end

    # call-seq:
    #    DateTime.strptime([string='-4712-01-01T00:00:00+00:00'[, format='%FT%T%z'[ ,start=Date::ITALY]]])  ->  datetime
    #
    # Parses the given representation of date and time with the given
    # template, and creates a DateTime object.  strptime does not support
    # specification of flags and width unlike strftime.
    #
    #    DateTime.strptime('2001-02-03T04:05:06+07:00', '%Y-%m-%dT%H:%M:%S%z')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #    DateTime.strptime('03-02-2001 04:05:06 PM', '%d-%m-%Y %I:%M:%S %p')
    #				#=> #<DateTime: 2001-02-03T16:05:06+00:00 ...>
    #    DateTime.strptime('2001-W05-6T04:05:06+07:00', '%G-W%V-%uT%H:%M:%S%z')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #    DateTime.strptime('2001 04 6 04 05 06 +7', '%Y %U %w %H %M %S %z')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #    DateTime.strptime('2001 05 6 04 05 06 +7', '%Y %W %u %H %M %S %z')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #    DateTime.strptime('-1', '%s')
    #				#=> #<DateTime: 1969-12-31T23:59:59+00:00 ...>
    #    DateTime.strptime('-1000', '%Q')
    #				#=> #<DateTime: 1969-12-31T23:59:59+00:00 ...>
    #    DateTime.strptime('sat3feb014pm+7', '%a%d%b%y%H%p%z')
    #				#=> #<DateTime: 2001-02-03T16:00:00+07:00 ...>
    #
    # See also strptime(3) and #strftime.
    def strptime(string = '-4712-01-01T00:00:00+00:00', format = '%FT%T%z', start = Date::ITALY)
      hash = _strptime(string, format)
      _dt_new_by_frags(hash, start)
    end

    # call-seq:
    #    DateTime.jd([jd=0[, hour=0[, minute=0[, second=0[, offset=0[, start=Date::ITALY]]]]]])  ->  datetime
    #
    # Creates a DateTime object denoting the given chronological Julian
    # day number.
    #
    #    DateTime.jd(2451944)	#=> #<DateTime: 2001-02-03T00:00:00+00:00 ...>
    #    DateTime.jd(2451945)	#=> #<DateTime: 2001-02-04T00:00:00+00:00 ...>
    #    DateTime.jd(Rational('0.5'))
    #				#=> #<DateTime: -4712-01-01T12:00:00+00:00 ...>
    def jd(jd = 0, hour = UNSET, minute = UNSET, second = UNSET, offset = 0, start = Date::ITALY)
      hour_given   = !UNSET.equal?(hour);   hour   = 0 if UNSET.equal?(hour)
      minute_given = !UNSET.equal?(minute); minute = 0 if UNSET.equal?(minute)
      second_given = !UNSET.equal?(second); second = 0 if UNSET.equal?(second)

      # Validate in the C extension's order (offset, second, minute, hour, jd).
      of_sec = _parse_of(offset)
      _check_hms_types(hour, minute, second)
      raise TypeError, "invalid jd (not numeric)" unless jd.is_a?(Numeric)

      jd_r = jd.to_r
      jd_i = jd_r.floor
      _check_time_fractions(jd_r - jd_i, hour, minute, second,
                            hour_given, minute_given, second_given)
      _build_dt_folded(jd_i, jd_r - jd_i, hour, minute, second, of_sec, start)
    end

    # call-seq:
    #    DateTime.ordinal([year=-4712[, yday=1[, hour=0[, minute=0[, second=0[, offset=0[, start=Date::ITALY]]]]]]])  ->  datetime
    #
    # Creates a DateTime object denoting the given ordinal date.
    #
    #    DateTime.ordinal(2001,34)	#=> #<DateTime: 2001-02-03T00:00:00+00:00 ...>
    #    DateTime.ordinal(2001,34,4,5,6,'+7')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #    DateTime.ordinal(2001,-332,-20,-55,-54,'+7')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    def ordinal(year = -4712, yday = 1, hour = UNSET, minute = UNSET, second = UNSET, offset = 0, start = Date::ITALY)
      hour_given   = !UNSET.equal?(hour);   hour   = 0 if UNSET.equal?(hour)
      minute_given = !UNSET.equal?(minute); minute = 0 if UNSET.equal?(minute)
      second_given = !UNSET.equal?(second); second = 0 if UNSET.equal?(second)

      # Validate in the C extension's order (offset, second, minute, hour, yday, year).
      of_sec = _parse_of(offset)
      _check_hms_types(hour, minute, second)
      raise TypeError, "invalid yday (not numeric)" unless yday.is_a?(Numeric)
      raise TypeError, "invalid year (not numeric)" unless year.is_a?(Numeric)

      yday_r = yday.to_r
      yday_i = yday_r.floor
      _check_time_fractions(yday_r - yday_i, hour, minute, second,
                            hour_given, minute_given, second_given)
      jd_v = internal_valid_ordinal?(Integer(year), yday_i, start)
      raise Date::Error, "invalid date" unless jd_v
      _build_dt_folded(jd_v, yday_r - yday_i, hour, minute, second, of_sec, start)
    end

    # call-seq:
    #    DateTime.commercial([cwyear=-4712[, cweek=1[, cwday=1[, hour=0[, minute=0[, second=0[, offset=0[, start=Date::ITALY]]]]]]]])  ->  datetime
    #
    # Creates a DateTime object denoting the given week date.
    #
    #    DateTime.commercial(2001)	#=> #<DateTime: 2001-01-01T00:00:00+00:00 ...>
    #    DateTime.commercial(2002)	#=> #<DateTime: 2001-12-31T00:00:00+00:00 ...>
    #    DateTime.commercial(2001,5,6,4,5,6,'+7')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    def commercial(cwyear = -4712, cweek = 1, cwday = 1, hour = UNSET, minute = UNSET, second = UNSET, offset = 0, start = Date::ITALY)
      hour_given   = !UNSET.equal?(hour);   hour   = 0 if UNSET.equal?(hour)
      minute_given = !UNSET.equal?(minute); minute = 0 if UNSET.equal?(minute)
      second_given = !UNSET.equal?(second); second = 0 if UNSET.equal?(second)

      # Validate in the C extension's order (offset, second, minute, hour,
      # cwday, cweek, cwyear).
      of_sec = _parse_of(offset)
      _check_hms_types(hour, minute, second)
      raise TypeError, "invalid cwday (not numeric)" unless cwday.is_a?(Numeric)
      raise TypeError, "invalid cweek (not numeric)" unless cweek.is_a?(Numeric)
      raise TypeError, "invalid year (not numeric)"  unless cwyear.is_a?(Numeric)

      cwday_r = cwday.to_r
      cwday_i = cwday_r.floor
      _check_time_fractions(cwday_r - cwday_i, hour, minute, second,
                            hour_given, minute_given, second_given)
      jd_v = internal_valid_commercial?(Integer(cwyear), Integer(cweek), cwday_i, start)
      raise Date::Error, "invalid date" unless jd_v
      _build_dt_folded(jd_v, cwday_r - cwday_i, hour, minute, second, of_sec, start)
    end

    # call-seq:
    #    DateTime.now([start=Date::ITALY])  ->  datetime
    #
    # Creates a DateTime object denoting the present time.
    #
    #    DateTime.now		#=> #<DateTime: 2011-06-11T21:20:44+09:00 ...>
    def now(start = Date::ITALY)
      t = Time.now
      jd = civil_to_jd(t.year, t.mon, t.mday, start)
      sec_f = Rational(t.subsec)
      _new_dt_from_jd_time(jd, t.hour, t.min, t.sec, sec_f, t.utc_offset, start)
    end

    # :nodoc:
    def _new_dt_from_jd_time(jd, h, m, s, sf, of, sg)
      jd, h, m, s = _normalize_hms(jd, h, m, s)
      obj = allocate
      obj.__send__(:_init_datetime, jd, h, m, s, sf, of, sg)
      obj
    end

    # Normalize hour/min/sec.
    # Negative values: add one period (non-cascading).
    # After normalization, validate ranges and raise Date::Error if out of range.
    # 24:00:00 is valid (normalizes to next day 00:00:00).
    def _normalize_hms(jd, h, m, s)
      s += 60 if s < 0
      m += 60 if m < 0
      h += 24 if h < 0
      raise Date::Error, "invalid date" if s >= 60
      raise Date::Error, "invalid date" if m >= 60
      raise Date::Error, "invalid date" if h >  24 || h < 0
      raise Date::Error, "invalid date" if h == 24 && (m != 0 || s != 0)
      if h == 24
        jd += 1
        h = 0
      end
      [jd, h, m, s]
    end

    # Type-check the time-of-day arguments shared by jd/ordinal/commercial, in
    # the C extension's order (second, then minute, then hour).
    def _check_hms_types(hour, minute, second)
      raise TypeError, "invalid second (not numeric)" unless second.is_a?(Numeric)
      raise TypeError, "invalid minute (not numeric)" unless minute.is_a?(Numeric)
      raise TypeError, "invalid hour (not numeric)"   unless hour.is_a?(Numeric)
    end

    # A fractional date field or hour/minute is only allowed as the last value
    # given; if a smaller unit was also supplied, the C extension rejects it.
    def _check_time_fractions(date_frac, hour, minute, second,
                              hour_given, minute_given, second_given)
      hr = hour.to_r
      mr = minute.to_r
      raise Date::Error, "invalid fraction" if date_frac != 0 && (hour_given || minute_given || second_given)
      raise Date::Error, "invalid fraction" if (hr - hr.floor) != 0 && (minute_given || second_given)
      raise Date::Error, "invalid fraction" if (mr - mr.floor) != 0 && second_given
    end

    # Fold the fractional date part and the hour/minute/second into normalized
    # time fields (a fraction cascades into the next smaller unit) and build.
    def _build_dt_folded(jd_i, date_frac, hour, minute, second, of_sec, start)
      hour_r   = hour.to_r + date_frac * 24
      h_i      = hour_r.floor
      minute_r = minute.to_r + (hour_r - h_i) * 60
      m_i      = minute_r.floor
      second_r = second.to_r + (minute_r - m_i) * 60
      sec_i    = second_r.floor
      sec_f    = second_r - sec_i
      _new_dt_from_jd_time(jd_i, h_i, m_i, sec_i, sec_f, of_sec, start)
    end

    # call-seq:
    #    DateTime.parse(string='-4712-01-01T00:00:00+00:00'[, comp=true[, start=Date::ITALY]], limit: 128)  ->  datetime
    #
    # Parses the given representation of date and time, and creates a
    # DateTime object.
    #
    # This method *does* *not* function as a validator.  If the input
    # string does not match valid formats strictly, you may get a cryptic
    # result.  Should consider to use DateTime.strptime instead of this
    # method as possible.
    #
    # If the optional second argument is true and the detected year is in
    # the range "00" to "99", makes it full.
    #
    #    DateTime.parse('2001-02-03T04:05:06+07:00')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #    DateTime.parse('20010203T040506+0700')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #    DateTime.parse('3rd Feb 2001 04:05:06 PM')
    #				#=> #<DateTime: 2001-02-03T16:05:06+00:00 ...>
    #
    # Raise an ArgumentError when the string length is longer than _limit_.
    # You can stop this check by passing <code>limit: nil</code>, but note
    # that it may take a long time to parse.
    def parse(string = '-4712-01-01T00:00:00+00:00', comp = true, start = Date::ITALY, limit: 128)
      hash = Date._parse(string, comp, limit: limit)
      _dt_new_by_frags(hash, start)
    end

    # call-seq:
    #    DateTime.iso8601(string='-4712-01-01T00:00:00+00:00'[, start=Date::ITALY], limit: 128)  ->  datetime
    #
    # Creates a new DateTime object by parsing from a string according to
    # some typical ISO 8601 formats.
    #
    #    DateTime.iso8601('2001-02-03T04:05:06+07:00')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #    DateTime.iso8601('20010203T040506+0700')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #    DateTime.iso8601('2001-W05-6T04:05:06+07:00')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #
    # Raise an ArgumentError when the string length is longer than _limit_.
    # You can stop this check by passing <code>limit: nil</code>, but note
    # that it may take a long time to parse.
    def iso8601(string = '-4712-01-01T00:00:00+00:00', start = Date::ITALY, limit: 128)
      hash = Date._iso8601(string, limit: limit)
      _dt_new_by_frags(hash, start)
    end

    # call-seq:
    #    DateTime.rfc3339(string='-4712-01-01T00:00:00+00:00'[, start=Date::ITALY], limit: 128)  ->  datetime
    #
    # Creates a new DateTime object by parsing from a string according to
    # some typical RFC 3339 formats.
    #
    #    DateTime.rfc3339('2001-02-03T04:05:06+07:00')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #
    # Raise an ArgumentError when the string length is longer than _limit_.
    # You can stop this check by passing <code>limit: nil</code>, but note
    # that it may take a long time to parse.
    def rfc3339(string = '-4712-01-01T00:00:00+00:00', start = Date::ITALY, limit: 128)
      hash = Date._rfc3339(string, limit: limit)
      _dt_new_by_frags(hash, start)
    end

    # call-seq:
    #    DateTime.xmlschema(string='-4712-01-01T00:00:00+00:00'[, start=Date::ITALY], limit: 128)  ->  datetime
    #
    # Creates a new DateTime object by parsing from a string according to
    # some typical XML Schema formats.
    #
    #    DateTime.xmlschema('2001-02-03T04:05:06+07:00')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #
    # Raise an ArgumentError when the string length is longer than _limit_.
    # You can stop this check by passing <code>limit: nil</code>, but note
    # that it may take a long time to parse.
    def xmlschema(string = '-4712-01-01T00:00:00+00:00', start = Date::ITALY, limit: 128)
      hash = Date._xmlschema(string, limit: limit)
      _dt_new_by_frags(hash, start)
    end

    # call-seq:
    #    DateTime.rfc2822(string='Mon, 1 Jan -4712 00:00:00 +0000'[, start=Date::ITALY], limit: 128)  ->  datetime
    #    DateTime.rfc822(string='Mon, 1 Jan -4712 00:00:00 +0000'[, start=Date::ITALY], limit: 128)   ->  datetime
    #
    # Creates a new DateTime object by parsing from a string according to
    # some typical RFC 2822 formats.
    #
    #     DateTime.rfc2822('Sat, 3 Feb 2001 04:05:06 +0700')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #
    # Raise an ArgumentError when the string length is longer than _limit_.
    # You can stop this check by passing <code>limit: nil</code>, but note
    # that it may take a long time to parse.
    def rfc2822(string = 'Mon, 1 Jan -4712 00:00:00 +0000', start = Date::ITALY, limit: 128)
      hash = Date._rfc2822(string, limit: limit)
      _dt_new_by_frags(hash, start)
    end
    alias rfc822 rfc2822

    # call-seq:
    #    DateTime.httpdate(string='Mon, 01 Jan -4712 00:00:00 GMT'[, start=Date::ITALY])  ->  datetime
    #
    # Creates a new DateTime object by parsing from a string according to
    # some RFC 2616 format.
    #
    #    DateTime.httpdate('Sat, 03 Feb 2001 04:05:06 GMT')
    #				#=> #<DateTime: 2001-02-03T04:05:06+00:00 ...>
    #
    # Raise an ArgumentError when the string length is longer than _limit_.
    # You can stop this check by passing <code>limit: nil</code>, but note
    # that it may take a long time to parse.
    def httpdate(string = 'Mon, 01 Jan -4712 00:00:00 GMT', start = Date::ITALY, limit: 128)
      hash = Date._httpdate(string, limit: limit)
      _dt_new_by_frags(hash, start)
    end

    # call-seq:
    #    DateTime.jisx0301(string='-4712-01-01T00:00:00+00:00'[, start=Date::ITALY], limit: 128)  ->  datetime
    #
    # Creates a new DateTime object by parsing from a string according to
    # some typical JIS X 0301 formats.
    #
    #    DateTime.jisx0301('H13.02.03T04:05:06+07:00')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #
    # For no-era year, legacy format, Heisei is assumed.
    #
    #    DateTime.jisx0301('13.02.03T04:05:06+07:00')
    #				#=> #<DateTime: 2001-02-03T04:05:06+07:00 ...>
    #
    # Raise an ArgumentError when the string length is longer than _limit_.
    # You can stop this check by passing <code>limit: nil</code>, but note
    # that it may take a long time to parse.
    def jisx0301(string = '-4712-01-01T00:00:00+00:00', start = Date::ITALY, limit: 128)
      hash = Date._jisx0301(string, limit: limit)
      _dt_new_by_frags(hash, start)
    end

    private

    # Create a DateTime object from parsed fragment hash.
    # Uses the same fragment rewrite/complete/validate logic as Date._new_by_frags
    # but additionally extracts time components (hour, min, sec, offset, sec_fraction).
    def _dt_new_by_frags(hash, sg)
      raise Date::Error, 'invalid date' if hash.nil?
      hash = sp_rewrite_frags(hash)
      orig_sec = hash[:sec]
      hash = sp_complete_frags(DateTime, hash)
      jd   = sp_valid_date_frags_p(hash, sg)
      raise Date::Error, 'invalid date' if jd.nil?

      h  = hash[:hour] || 0
      m  = hash[:min]  || 0
      s  = hash[:sec]  || 0
      raise Date::Error, 'invalid date' if orig_sec && orig_sec > 60
      s  = 59 if s > 59
      of = hash[:offset] || 0
      if of.is_a?(Numeric) && (of < -86400 || of > 86400)
        warn("invalid offset is ignored: #{of}", uplevel: 0)
        of = 0
      end
      sf = hash[:sec_fraction] || 0r
      _new_dt_from_jd_time(jd, h, m, s, sf, of, sg)
    end

    def _parse_of(offset)
      case offset
      when String
        Date.__send__(:offset_str_to_sec, offset)
      when Integer
        # Only whole-day offsets of -1, 0, or 1 are valid; anything else is
        # ignored (offset 0), matching the C extension.
        return offset * 86400 if offset == -1 || offset == 0 || offset == 1
        warn("invalid offset is ignored", uplevel: 0) if $VERBOSE
        0
      when Float
        # Valid only within +/- one day; out of range is ignored. A fractional
        # second is rounded (with a warning), matching the C extension.
        sec = offset * 86400
        if sec < -86400 || sec > 86400
          warn("invalid offset is ignored", uplevel: 0) if $VERBOSE
          return 0
        end
        n = sec.round
        warn("fraction of offset is ignored", uplevel: 0) if $VERBOSE && n != sec
        n
      when Numeric
        # Rational (and other numerics via to_r). Convert to seconds, round a
        # fractional value (with a warning), then range-check against +/- one
        # day; out of range is ignored, matching the C extension.
        r = offset.to_r
        raise TypeError, "#{offset.class} can't be used as offset" unless r.is_a?(Rational)
        sec = r * 86400
        if sec.denominator == 1
          n = sec.numerator
        else
          n = sec.round
          warn("fraction of offset is ignored", uplevel: 0) if $VERBOSE
        end
        if n < -86400 || n > 86400
          warn("invalid offset is ignored", uplevel: 0) if $VERBOSE
          return 0
        end
        n
      else
        raise TypeError, "expected numeric"
      end
    end

  end

  # ---------------------------------------------------------------------------
  # Private helpers (strftime overrides)
  # ---------------------------------------------------------------------------

  private

  # Coerce n to an Integer the way the C extension's NUM2INT does: accept an
  # Integer or anything with #to_int (e.g. Float, Rational), and otherwise
  # raise TypeError rather than falling back to #to_i.
  def _num_to_int(n)
    return n if Integer === n
    return n.to_int if n.respond_to?(:to_int)
    raise TypeError, n.nil? ? "no implicit conversion from nil to integer" :
                              "no implicit conversion of #{n.class} into Integer"
  end

  def internal_hour
    @hour
  end

  def internal_min
    @min
  end

  def internal_sec
    @sec
  end

  def sec_frac
    @sec_fraction
  end

  def of_seconds
    @of
  end

  def zone_str
    zone
  end

  # Overrides of the Date strftime time-part helpers (see Date#strftime_hms):
  # a DateTime renders its actual time instead of the Date literals.
  def strftime_hms
    "#{PAD2[@hour]}:#{PAD2[@min]}:#{PAD2[@sec]}"
  end

  def strftime_hm
    "#{PAD2[@hour]}:#{PAD2[@min]}"
  end

  def strftime_r12
    h = @hour % 12
    h = 12 if h == 0
    "#{PAD2[h]}:#{PAD2[@min]}:#{PAD2[@sec]} #{@hour < 12 ? 'AM' : 'PM'}"
  end

  def strftime_zoff
    of = @of
    sign = of < 0 ? '-' : '+'
    abs = of.abs
    "#{sign}#{PAD2[abs / 3600]}#{PAD2[(abs % 3600) / 60]}"
  end

  def _init_datetime(jd, h, m, s, sf, of, sg)
    @jd            = Integer(jd)
    # Store the cutover as a Float so #start/#hash/marshal stay consistent
    # (see Date#initialize).
    @start         = Float(sg.to_f)
    @day_fraction  = nil
    @hour          = Integer(h)
    @min           = Integer(m)
    @sec           = Integer(s)
    @sec_fraction  = Rational(sf)
    @of            = of.to_i
    # Initialize lazily-computed (inherited from Date) civil ivars up front so
    # every DateTime instance shares a single object shape.
    @year          = nil
    @month         = nil
    @day           = nil
  end

  def _split_second(second)
    if second.is_a?(Rational) || second.is_a?(Float)
      s_r = second.to_r
      s_i = s_r.floor
      [s_i, s_r - s_i]
    else
      [Integer(second), 0r]
    end
  end

  def _from_total_sec_r(total_r)
    jd  = (total_r / 86400).floor
    rem = total_r - jd * 86400
    h   = rem.to_i / 3600
    rem -= h * 3600
    m   = rem.to_i / 60
    s_r = rem - m * 60
    s_i = s_r.floor
    s_f = s_r - s_i
    self.class.__send__(:_new_dt_from_jd_time,jd, h, m, s_i, s_f, @of, @start)
  end

end
