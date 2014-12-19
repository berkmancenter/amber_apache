#! /bin/bash

sudo apt-get update
sudo apt-get -y install git make curl libpcre3 libpcre3-dev sqlite3 libsqlite3-dev php5-cli php5-common php5-sqlite php5-curl php5-fpm zlib1g-dev apache2 apache2-dev libapache2-mod-php5

cd /usr/local/src
sudo git clone https://github.com/berkmancenter/amber_common.git
#sudo git clone https://github.com/berkmancenter/amber_apache.git

cd amber_apache
apxs -i -a -c mod_amber.c -lsqlite3 -lpcre

sudo cp /usr/local/src/amber_apache/amber.conf /etc/apache2/conf-available
sudo /usr/sbin/a2enmod rewrite
sudo /usr/sbin/a2enmod substitute
sudo /usr/sbin/a2enconf amber.conf


sudo mkdir /var/lib/amber /var/www/html/amber /var/www/html/amber/cache
sudo touch /var/log/amber
sudo ln -s /usr/local/src/amber_common/src/admin /var/www/html/amber/admin
sudo cp -r /usr/local/src/amber_common/src/css /usr/local/src/amber_common/src/js /var/www/html/amber

sudo sqlite3 /var/lib/amber/amber.db < /usr/local/src/amber_common/src/amber.sql
sudo cat > /etc/cron.d/amber << EOF
*/5 * * * * www-data /bin/sh /usr/local/src/amber_common/deploy/apache/vagrant/cron-cache.sh --ini=/usr/local/src/amber_common/src/amber-apache.ini 2>> /var/log/amber >> /var/log/amber
15 3 * * *  www-data /bin/sh /usr/local/src/amber_common/deploy/apache/vagrant/cron-check.sh --ini=/usr/local/src/amber_common/src/amber-apache.ini 2>> /var/log/amber >> /var/log/amber
EOF

sudo chgrp -R www-data /var/lib/amber /var/www/html/amber
sudo chmod -R g+w /var/lib/amber /var/www/html/amber/cache
sudo chmod +x /usr/local/src/amber_common/deploy/apache/vagrant/cron-cache.sh /usr/local/src/amber_common/deploy/apache/vagrant/cron-check.sh
sudo chown www-data /var/log/amber
sudo chgrp www-data /var/log/amber