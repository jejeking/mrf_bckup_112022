FROM centos:8

COPY rpms/gibs-gdal-*.el8.*.rpm /rpms/

RUN yum install -y epel-release python36 && yum clean all
RUN yum install -y /rpms/gibs-gdal-*.el8.*.rpm
