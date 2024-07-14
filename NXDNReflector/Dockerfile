# Use an official base image with build tools pre-installed
FROM ubuntu:latest

# Install necessary packages
RUN apt-get update && apt-get install -y \
    gcc \
    g++ \
    make \
    && rm -rf /var/lib/apt/lists/*

# Set the working directory in the container
WORKDIR /usr/src/app

# Copy the local code to the container's working directory
COPY . .

# Copy the NXDNReflector.ini configuration file to /etc
COPY NXDNReflector.ini /etc/NXDNReflector.ini

# Use the Makefile to build the application
RUN make all

# Expose port 41400 for UDP traffic
EXPOSE 41400/udp

# The command to run when the container starts
CMD ["./NXDNReflector"]
