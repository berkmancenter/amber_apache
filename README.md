# Amber apache plugin #

[![Build Status](https://travis-ci.org/berkmancenter/amber_apache.png?branch=master)](https://travis-ci.org/berkmancenter/amber_apache)

This is Amber, an Apache module that provides an alternative route to information when content would otherwise be unavailable. Amber is useful for virtually any organization or individual that has an interest in preserving the content to which their website links.

If you’d like to join the private beta, we welcome critiques and feedback as we progress through testing. As part of the beta, the Berkman Center will incorporate your suggestions to see what works, what doesn't, and what can be improved. You will also receive personal help and support from our team of devs and sysadmins in running Amber on Apache.

Indicate your interest by contacting amber@cyber.law.harvard.edu.

## System Requirements ##

* git
* sqlite3
* PHP 5.3 or higher
* cURL
* php-fpm

## Installation (Ubuntu) ##

The Amber plugin consists of two components:

* An **Apache module** that identifies pages to be cached, schedules them for caching, and links to cached pages
* A **caching script** that runs periodically to cache new pages and check on the status of existing pages

#### Representations ####
This documentation contains the following representations:
* **$BUILDDIR** -We represent the build directory as $BUILDDIR. (For example, in our install our build directory was /usr/local/src)
* **$DATADIR** -We represent the data directory as $DATADIR. (For example, in our install our data directory was /var/lib)
* **$LOGDIR** -We represent the log directory as $LOGDIR. (For example, in our install our log directory was /var/log)
* **$WEBROOT** -We represent the default path as $WEBROOT. (For example, in our install our path was /var/www/html)

Note that the Amber module reserves the /amber top-level directory. If you already have a top-level directory called Amber, you must modify the instructions. (This refers to the top level directory from the perspective of the web server URLs, *not* the top level directory on the server.)

### Install procedure ###

Install prerequisites

    sudo apt-get update
    sudo apt-get -y install git make curl libpcre3 libpcre3-dev sqlite3 libsqlite3-dev php5-cli php5-common php5-sqlite php5-curl php5-fpm zlib1g-dev apache2 apache2-dev libapache2-mod-php5

Get code

    cd $BUILDDIR
    sudo git clone https://github.com/berkmancenter/amber_common.git
    sudo git clone https://github.com/berkmancenter/amber_apache.git

Build module

    cd amber_apache
    apxs -i -a -c mod_amber.c -lsqlite3 -lpcre

Install module

    sudo cp $BUILDDIR/amber_apache/amber.conf /etc/apache2/conf-available
    sudo /usr/sbin/a2enmod rewrite
    sudo /usr/sbin/a2enmod substitute
    sudo /usr/sbin/a2enconf amber.conf

Create Amber directories and install supporting files

    sudo mkdir $DATADIR/amber $WEBROOT/amber $WEBROOT/amber/cache
    sudo touch $LOGDIR/amber
    sudo ln -s $BUILDDIR/amber_common/src/admin $WEBROOT/amber/admin
    sudo cp -r $BUILDDIR/amber_common/src/css $BUILDDIR/amber_common/src/js $WEBROOT/amber

Create amber database and cron jobs

    sudo sqlite3 $DATADIR/amber/amber.db < $BUILDDIR/amber_common/src/amber.sql
    sudo cat > /etc/cron.d/amber << EOF
    */5 * * * * $WEBROLE /bin/sh $BUILDDIR/amber_common/deploy/apache/vagrant/cron-cache.sh --ini=$BUILDDIR/amber_common/src/amber-apache.ini 2>> $LOGDIR/amber >> $LOGDIR/amber
    15 3 * * *  $WEBROLE /bin/sh $BUILDDIR/amber_common/deploy/apache/vagrant/cron-check.sh --ini=$BUILDDIR/amber_common/src/amber-apache.ini 2>> $LOGDIR/amber >> $LOGDIR/amber
    EOF

Update permissions

    sudo chgrp -R $WEBROLE $DATADIR/amber $WEBROOT/amber
    sudo chmod -R g+w $DATADIR/amber $WEBROOT/amber/cache
    sudo chmod +x $BUILDDIR/amber_common/deploy/apache/vagrant/cron-cache.sh $BUILDDIR/amber_common/deploy/apache/vagrant/cron-check.sh
    sudo chown $WEBROLE $LOGDIR/amber
    sudo chgrp $WEBROLE $LOGDIR/amber

Add the following configuration settings to your virtual hosts configuration file:

    RewriteEngine on
    RewriteRule ^/amber/cache/([a-fA-F0-9]+)/$ /amber/cache/$1/$1 [T=text/html]
    RewriteRule ^/amber/admin/$ /amber/admin/reports.php [PT]

    <LocationMatch /amber/cache/[a-fA-F0-9]+/$>
        <IfModule amber_module>
            AmberEnabled off
            AmberCacheDelivery on
        </IfModule>
    </LocationMatch>

    # Configuration settings required to display the admin page

    <Location /amber/admin>
        <IfModule amber_module>
            AmberEnabled off
            SetEnv AMBER_CONFIG "$BUILDDIR/amber_common/src/amber-apache.ini"
        </IfModule>
    </Location>

Reload apache

    sudo service apache2 reload    

## Troubleshooting - Apache plugin ##

The deflate module can prevent the substitute module from working properly, if they are run in the wrong order. If the Amber javascript and CSS are not being inserted properly, try disabling deflate:

    /usr/sbin/a2dismod deflate


## Configuration - Apache plugin ##

The Amber apache plugin uses the following configuration directives. See the provided amber.conf for examples. 

Enable Amber

    SetOutputFilter amber-filter
    AmberEnabled <on|off>

The location of the sqlite3 database file used by Amber to keep track of cached links

    AmberDatabase <filename>;

The behavior for cached links that appear to be down

    AmberBehaviorDown <hover|popup|cache>;

The behavior for cached links that appear to be up

    AmberBehaviorUp <hover|popup|cache>;

If the behavior is "hover", the delay in seconds before displaying the hover popup. There are separate configurations for liks that are up or down

    AmberHoverDelayUp <time in seconds>;
    AmberHoverDelayDown <time in seconds>;

Specific behavior can be specified for an additional country. The country must be identified, using the ISO code (e.g. US, DE). 

    AmberCountry <country_code>

The behavior for the country is set using the usual directives, prefixed by ```country_```

    AmberCountryBehaviorDown <hover|popup|cache>;
    AmberCountryBehaviorUp <hover|popup|cache>;
    AmberCountryHoverDelayUp <time in seconds>;
    AmberCountryHoverDelayDown <time in seconds>;

Insert Javascript and CSS required for Amber to function. `Required`

    AddOutputFilterByType SUBSTITUTE text/html
    Substitute "s|</head>|<script type='text/javascript' src='/amber/js/amber.js'></script><link rel='stylesheet' type='text/css' href='/amber/css/amber.css'></head>|niq"

Allow access to the Amber Admin page to multiple users

    Coming soon!

Create user agent name custom to your domain

    Coming soon!

Modify cron processes (keep caches as-is, or update regularly in "live mirror" format)

    Coming soon!

## Security configuration ##

For improved security, you must next configure Amber to serve cached content from a separate domain. We recommend that you establish a subdomain for this purpose.

Here is a sample rewrite rule where the site is running at www.amber.com, while cached content is served from sandbox.amber.com.

    RewriteCond %{HTTP_HOST} ^www.amber.com$
    RewriteRule /amber/cache/[a-fA-F0-9]+/$ http://sandbox.amber.com%{REQUEST_URI} [R=301,L]

## Configuration - Caching ##

The caching process is configured through ```amber.ini``` - full documentation is available within the sample configuration file [here](https://github.com/berkmancenter/amber_common/blob/master/src/amber.ini) 

## Optional configuration ##
Display Farsi version of Javascript and CSS

    AddOutputFilterByType SUBSTITUTE text/html
    Substitute "s|</head>|<script type="text/javascript">var amber_locale="fa";</script><script type="text/javascript" src="/amber/js/amber.js"></script><link rel="stylesheet" type="text/css" href="/amber/css/amber.css"><link rel="stylesheet" type="text/css" href="/amber/css/amber_fa.css"></head>|niq"
