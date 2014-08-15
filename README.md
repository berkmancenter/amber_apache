# Amber apache plugin #

[![Build Status](https://travis-ci.org/berkmancenter/robustness_apache.png?branch=master)](https://travis-ci.org/berkmancenter/robustness_apache)

The Amber plugin consists of two components:

* An apache module that identifies pages to be cached, schedules them for caching, and links to cached pages
* A caching script that runs periodically to cache new pages and check on the status of existing pages

## System Requirements ##

* sqlite3
* PHP 5.3 or higher
* cURL
* php-fpm

## Installation (Ubuntu) ##

Install prerequisites

    sudo apt-get update
    sudo apt-get -y install git make curl libpcre3 libpcre3-dev sqlite3 libsqlite3-dev php5-cli php5-common php5-sqlite php5-curl php5-fpm zlib1g-dev apache2 apache2-dev libapache2-mod-php5

Get code

    cd /usr/local/src
    sudo git clone https://github.com/berkmancenter/robustness_common.git
    sudo git clone https://github.com/berkmancenter/robustness_apache.git

Build module

    cd robustness_apache
    apxs -i -a -c mod_amber.c -lsqlite3 -lpcre

Install module

    sudo cp /usr/local/src/robustness_apache/amber.conf /etc/apache2/conf-available
    sudo /usr/sbin/a2enmod rewrite
    sudo /usr/sbin/a2enmod substitute
    sudo /usr/sbin/a2enconf amber.conf

Create amber directories and install supporting files

    sudo mkdir /var/lib/amber /var/www/html/amber /var/www/html/amber/cache
    sudo touch /var/log/amber
    sudo ln -s /usr/local/src/robustness_common/src/admin /var/www/html/amber/admin
    sudo cp -r /usr/local/src/robustness_common/src/css /usr/local/src/robustness_common/src/js /var/www/html/amber

Create amber database and cron jobs

    sudo sqlite3 /var/lib/amber/amber.db < /usr/local/src/robustness_common/src/amber.sql
    sudo cat > /etc/cron.d/amber << EOF
    */5 * * * * www-data /bin/sh /usr/local/src/robustness_common/deploy/apache/vagrant/cron-cache.sh --ini=/usr/local/src/robustness_common/src/amber-apache.ini 2>> /var/log/amber >> /var/log/amber
    15 3 * * *  www-data /bin/sh /usr/local/src/robustness_common/deploy/apache/vagrant/cron-check.sh --ini=/usr/local/src/robustness_common/src/amber-apache.ini 2>> /var/log/amber >> /var/log/amber
    EOF

Update permissions

    sudo chgrp -R www-data /var/lib/amber /var/www/html/amber
    sudo chmod -R g+w /var/lib/amber /var/www/html/amber/cache
    sudo chmod +x /usr/local/src/robustness_common/deploy/apache/vagrant/cron-cache.sh /usr/local/src/robustness_common/deploy/apache/vagrant/cron-check.sh
    sudo chown www-data /var/log/amber
    sudo chgrp www-data /var/log/amber

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
            SetEnv AMBER_CONFIG "/usr/local/src/robustness_common/src/amber-apache.ini"
        </IfModule>
    </Location>

Reload apache

    sudo service apache2 reload    

## Troublshooting - Apache plugin ##

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

Display Farsi version of Javascript and CSS 

    AddOutputFilterByType SUBSTITUTE text/html
    Substitute "s|</head>|<script type="text/javascript">var amber_locale="fa";</script><script type="text/javascript" src="/amber/js/amber.js"></script><link rel="stylesheet" type="text/css" href="/amber/css/amber.css"><link rel="stylesheet" type="text/css" href="/amber/css/amber_fa.css"></head>|niq"

## Configuration - Caching ##

The caching process is configured through ```amber.ini``` - full documentation is available within the sample configuration file [here](https://github.com/berkmancenter/robustness_common/blob/master/src/amber.ini) 