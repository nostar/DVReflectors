# Use an official base image with build tools pre-installed
FROM ubuntu:latest

# Install necessary packages
RUN apt-get update && apt-get install -y \
    gcc \
    g++ \
    make \
    && rm -rf /var/lib/apt/lists/*

# Create a group and user 'mmdvm'
RUN groupadd mmdvm && \
    useradd mmdvm -g mmdvm -s /sbin/nologin && \
    usermod -a -G dialout mmdvm

# Set the working directory in the container
WORKDIR /usr/src/app

# Copy the local code to the container's working directory
COPY . .

# Copy the NXDNReflector.ini configuration file to /etc
COPY NXDNReflector.ini /etc/NXDNReflector.ini

# Adjust permissions for the copied files and change owner to mmdvm
RUN chown -R mmdvm:mmdvm /usr/src/app /etc/NXDNReflector.ini

# Use the Makefile to build the application
RUN make all

# Expose port 41400 for UDP traffic
EXPOSE 41400/udp

# Run the application as the 'mmdvm' user
USER mmdvm

# The command to run when the container starts
CMD ["./NXDNReflector"]
